// What querying and walking a tree give in a page: which selectors parse and
// what each that does not throws, which elements a selector matches in a
// fixture of forms, links, directions, languages and foreign content, the
// collections that the DOM's methods give, and what Range, StaticRange,
// TreeWalker and NodeIterator do and throw. tests/test_page_queries.py runs
// it in the Page program on the markup of _PAGE there and compares with what
// Chrome 157.0.8103.51 gave for the same script on the same page, from a new
// profile, in tests/data/query_cases.json. Its value is the record as JSON.

(() => {
  const outcome = (f) => {
    try {
      return f();
    } catch (error) {
      return { threw: `${error.name}: ${error.message}` };
    }
  };

  const fixture = (markup) => {
    const root = document.createElement("div");
    root.innerHTML = markup;
    document.body.append(root);
    return root;
  };

  // The position of each element under `root`, in tree order, to name an
  // element the same way in both browsers.
  const names = (root, elements) => {
    const all = [...root.getElementsByTagName("*")];
    return [...elements].map((element) =>
      element === root ? "root" : element === null ? null : all.indexOf(element)
    );
  };

  // A boundary point as a path of child indices from the body, then the
  // offset.
  const point = (node, offset) => {
    const path = [];
    for (let n = node; n && n !== document.body; n = n.parentNode) {
      path.unshift(n.parentNode ? [...n.parentNode.childNodes].indexOf(n) : "detached");
    }
    return `${path.join(".")}@${offset}`;
  };
  const describeRange = (range) =>
    `${point(range.startContainer, range.startOffset)}-${point(range.endContainer, range.endOffset)}`;
  const markupOf = (fragment) => {
    const holder = document.createElement("div");
    holder.append(fragment.cloneNode(true));
    return holder.innerHTML;
  };

  const record = {};

  // Which selectors parse, and the error of each that does not.
  const selectors = [
    "*", "div", "DIV", "#id", ".class", "[attr]", "[attr=value]", "[attr='v' i]", "[attr=v s]", "[attr=v x]",
    "[attr~=v]", "[attr|=v]", "[attr^=v]", "[attr$=v]", "[attr*=v]", "[*|attr]", "[|attr]", "[ns|attr]", "[*]",
    "*|div", "|div", "ns|div", "*|*", "|*", "div|", "a b", "a > b", "a + b", "a ~ b", "a || b", "a >> b",
    "a,b", "a, b ,c", "a,", ",a", "a >", "> a", "a ~", "a b > c + d ~ e",
    ":is(a, b)", ":is()", ":is(!, a)", ":is(,)", ":where(a b, c > d)", ":where(::before)", ":not(a)", ":not()",
    ":not(a, b)", ":not(:not(a))", ":not(::before)", ":is(a):is(b)", ":-webkit-any(a, b)", ":-webkit-any(a b)",
    ":has(a)", ":has(> a)", ":has(+ a)", ":has(~ a)", ":has(a, > b)", ":has()", ":has(:has(a))",
    ":has(::before)", ":has(:is(:has(a)))", ":is(:has(a))", "::before:has(a)", "::part(x):has(a)",
    ":nth-child(2)", ":nth-child(2n+1)", ":nth-child(odd)", ":nth-child(EVEN)", ":nth-child(-n+3)",
    ":nth-child(n)", ":nth-child(+n)", ":nth-child(- n)", ":nth-child(2n + 1)", ":nth-child(2n- 1)",
    ":nth-child(2n -1)", ":nth-child(2n+ 1)", ":nth-child(2n+-1)", ":nth-child(n-1)", ":nth-child(-n-1)",
    ":nth-child(1.5)", ":nth-child(2n of a)", ":nth-child(2n OF a)", ":nth-child(2n of)", ":nth-of-type(2n of a)",
    ":nth-last-child(2 of a, b)", ":nth-child()", ":nth-child(a)", ":nth-child(3n+99999999999)",
    ":first-child", ":last-child", ":only-child", ":first-of-type", ":last-of-type", ":only-of-type",
    ":empty", ":root", ":scope", "&", "& > a", ":scope > a", ":link", ":any-link", ":-webkit-any-link",
    ":visited", ":hover", ":active", ":focus", ":focus-visible", ":focus-within", ":target", ":checked",
    ":default", ":indeterminate", ":disabled", ":enabled", ":required", ":optional", ":read-only",
    ":read-write", ":placeholder-shown", ":valid", ":invalid", ":user-valid", ":user-invalid", ":in-range",
    ":out-of-range", ":defined", ":open", ":popover-open", ":modal", ":fullscreen", ":-webkit-full-screen",
    ":picture-in-picture", ":autofill", ":-webkit-autofill", ":playing", ":paused", ":muted",
    ":lang(en)", ":lang('en')", ":lang(en, fr)", ":lang()", ":dir(ltr)", ":dir(foo)", ":dir()", ":state(x)",
    ":host", ":host(a)", ":host(a b)", ":host-context(a)", ":unknown", ":unknown()", ":is", ":hover()",
    "::before", ":before", "::BEFORE", "::after", "::marker", "::placeholder", "::selection", "::backdrop",
    "::first-line", ":first-letter", "::part(a b)", "::part()", "::slotted(a)", "::slotted(a b)", "::cue",
    "::cue(a)", "::picker(select)", "::picker(foo)", "::highlight(x)", "::highlight(*)",
    "::scroll-button(up)", "::scroll-button(*)", "::scroll-marker", "::column", "::column::scroll-marker",
    "::view-transition", "::view-transition-group(*)", "::view-transition-group(a.b)",
    "::view-transition-group(default)", "::-webkit-scrollbar", "::-webkit-scrollbar:horizontal",
    "::-webkit-foo", "::-webkit-foo()", "::-internal-foo", ":-internal-relative-anchor", ":-internal-is-html",
    ":-internal-autofill-selected", "::before::after", "::before.a", "::before:hover", "::before div",
    "::placeholder:hover", "::selection:window-inactive", "::part(x)::before", "::part(x):hover",
    "::part(x):first-child", "::slotted(a)::before", "div::before", ":checked::before",
    ":active-view-transition-type(a, b)", ":active-view-transition-type()", ":-internal-overscroll-area-parent(*)",
    "a/**/b", "a /**/ b", "a\\:b", "\\61", "#1", "#-", "#\\31", ".1", ".-a", "[a=1]", "[a='\n']",
    "div:nth-child(2n+1 of p:not(.x)) > span:is(.a, .b)::before",
  ];
  record.selectors = selectors.map((selector) => {
    const result = outcome(() => (document.body.querySelector(selector), "valid"));
    return [selector, result];
  });
  record["empty selector"] = outcome(() => document.querySelector(""));
  record["selector of a whitespace"] = outcome(() => document.querySelector("  "));

  // Which elements a selector matches.
  const root = fixture(`
    <section id=s lang=en-US dir=rtl>
      <p class="a B" id=p1>One <b>bold</b> two</p>
      <p class=b id=p2 title="hello world" data-x="Foo-bar"></p>
      <p id=p3> </p>
      <div id=d1><span>x</span><span class=a>y</span><i></i><span lang=fr>z</span></div>
      <a href=/x id=a1>link</a><a id=a2 name=n>anchor</a><area href=/y><img usemap=#m alt=""><img alt="">
      <link href=/z rel=stylesheet>
      <ul><li>1</li><li class=a>2</li><li>3</li><li class=a>4</li><li>5</li></ul>
      <bdi>שלום</bdi><span dir=auto>abc</span><span dir=auto>שלום abc</span><span dir=ltr>x<span>y</span></span>
      <custom-thing></custom-thing><div is=x-y></div><foo-></foo->
      <details open><summary>s</summary></details><dialog open></dialog><dialog></dialog>
      <progress></progress><progress value=1></progress>
      <div contenteditable><span>e</span><span contenteditable=false>n</span></div>
      <span id=svgslot></span>
    </section>
    <form id=f>
      <input id=i1 required>
      <input id=i2 value=x required>
      <input id=i3 type=checkbox checked>
      <input id=i4 type=checkbox required>
      <input type=radio name=r id=r1 checked><input type=radio name=r id=r2 checked><input type=radio name=r id=r3>
      <input type=radio name=q id=q1 required><input type=radio name=q id=q2>
      <input type=radio id=lone>
      <input id=i5 disabled><input id=i6 readonly><input type=hidden><input type=submit id=sub1><input type=submit>
      <input type=email value="a@b.c"><input type=email value="not an email"><input type=url value="nope">
      <input type=url value="https://x.test/"><input pattern="[a-z]+" value="abc"><input pattern="[a-z]+" value="123">
      <input type=number min=1 max=5 value=3><input type=number min=1 max=5 value=9><input type=number value=4>
      <input type=number min=0 step=2 value=3><input type=number value="oops" placeholder=n>
      <input type=range><input type=range min=0 max=10 step=2 value=3 required><input type=range min=1 max=10 step=4 value=99>
      <input type=range min=0 max=3 step=10 value=2><input type=range step=any value=7.5><input type=date min=2020-01-01 value=2019-12-31><input type=time min=22:00 max=02:00 value=23:00>
      <input placeholder=p><input placeholder=p value=v><textarea placeholder=t></textarea><textarea>v</textarea>
      <textarea required></textarea>
      <select id=s1><option>a<option selected>b<option selected>c</select>
      <select id=s2><option disabled>a<option>b</select>
      <select id=s3 required><option value="">pick<option>x</select>
      <select id=s4 multiple><option>a<option selected>b</select>
      <select id=s5 size=3><option>a<option>b</select>
      <select required id=s6><option value=" ">pick</option><option>x</option></select>
      <select required id=s7><option> </option><option>x</option></select>
      <fieldset disabled><legend><input id=inlegend></legend><input id=infield></fieldset>
      <fieldset disabled><input id=nolegend required></fieldset>
      <fieldset disabled><div><legend><input id=notfirst></legend></div></fieldset>
      <optgroup disabled><option>o</option></optgroup>
      <button id=b1>b</button><button type=reset>r</button>
    </form>
    <input form=f id=outside type=submit>`);
  // SVG, made by createElementNS() rather than markup, so that the cases do
  // not depend on the parser's foreign content.
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  const svg_link = document.createElementNS("http://www.w3.org/2000/svg", "a");
  svg_link.setAttribute("href", "#q");
  const svg_text = document.createElementNS("http://www.w3.org/2000/svg", "text");
  svg_text.textContent = "t";
  svg_link.append(svg_text);
  const foreign = document.createElementNS("http://www.w3.org/2000/svg", "foreignObject");
  const foreign_div = document.createElement("div");
  foreign_div.id = "fo";
  foreign_div.textContent = "f";
  foreign.append(foreign_div);
  const xlink = document.createElementNS("http://www.w3.org/2000/svg", "a");
  xlink.setAttributeNS("http://www.w3.org/1999/xlink", "xlink:href", "#r");
  svg.append(svg_link, foreign, xlink);
  root.querySelector("#svgslot").replaceWith(svg);
  const matching = [
    "*", "p", "P", "#p1", ".a", ".B", ".b", "[title]", "[title~=world]", "[data-x|=foo]", "[data-x|=Foo]",
    "[data-x^=foo i]", "[lang|=en]", "[dir=RTL]", "section p", "section > p", "p + p", "p ~ div", "div span",
    "li:nth-child(2n)", "li:nth-child(odd)", "li:nth-last-child(2)", "li:nth-child(2 of .a)", "li:nth-last-child(1 of .a)",
    "span:first-child", "span:last-child", "span:only-child", "i:only-of-type", "span:nth-of-type(2)",
    "span:nth-last-of-type(1)", "p:empty", "i:empty", ":is(b, i)", ":where(#p1) b", ":not(p, span, li, input, option)",
    "p:has(b)", "div:has(> span.a)", "p:has(+ p)", "p:has(~ div)", "section:has(li.a)", ":has(> li:nth-child(5))",
    ":root", ":scope", ":scope > section", "& > form", ":link", ":any-link", ":visited", ":hover", ":focus",
    ":target", ":checked", ":default", ":indeterminate", ":disabled", ":enabled", ":required", ":optional",
    ":read-only", ":read-write", ":placeholder-shown", ":valid", ":invalid", ":in-range", ":out-of-range",
    ":defined", ":not(:defined)", ":open", ":popover-open", ":modal", ":lang(en)", ":lang(EN-us)", ":lang(fr)",
    ":lang(e)", ":dir(rtl)", ":dir(ltr)", "svg a", "svg|a", "*|a", "|a", "text", "foreignObject",
    "foreignobject", "FOREIGNOBJECT", "foreignObject div", "#fo:dir(rtl)", "[href]", "[*|href]", "a[href]", "svg :link", ":-internal-text-field", "option:checked",
    "fieldset :disabled", "select:invalid", "select:valid", "form:invalid", "fieldset:valid",
    "input[type=range]:valid", "input[type=range]:in-range", "input[type=range]:invalid",
  ];
  record.matching = matching.map((selector) => [
    selector,
    outcome(() => names(root, root.querySelectorAll(selector))),
  ]);
  const p1 = root.querySelector("#p1");
  record["matches and closest"] = [
    outcome(() => p1.matches(":scope")),
    outcome(() => p1.matches("section > :scope")),
    outcome(() => names(root, [p1.querySelector(":scope > b")])),
    outcome(() => names(root, [p1.querySelector(":scope b")])),
    outcome(() => names(root, [p1.querySelector("section b")])),
    outcome(() => names(root, [p1.closest("section")])),
    outcome(() => names(root, [p1.closest(":scope")])),
    outcome(() => names(root, [p1.closest(":not(:scope)")])),
    outcome(() => p1.webkitMatchesSelector("p.a")),
    outcome(() => p1.matches("::before")),
    outcome(() => p1.matches("p, ::before")),
    outcome(() => document.createElement("div").matches(":first-child:last-child:only-child")),
    outcome(() => document.createElement("div").matches(":nth-child(1)")),
    outcome(() => document.documentElement.matches(":first-child")),
  ];
  // :defined follows the is value an element was made with, not its is
  // attribute now.
  const parsed = (html) => {
    const holder = document.createElement("div");
    holder.innerHTML = html;
    return holder.firstChild;
  };
  const html = "http://www.w3.org/1999/xhtml";
  record[":defined and is"] = [
    outcome(() => {
      const element = document.createElement("div");
      element.setAttribute("is", "x-y");
      return element.matches(":defined");
    }),
    outcome(() => parsed('<div is="x-y"></div>').matches(":defined")),
    outcome(() => {
      const element = parsed('<div is="x-y"></div>');
      element.removeAttribute("is");
      return element.matches(":defined");
    }),
    outcome(() => parsed('<div is="x-y"></div>').cloneNode().matches(":defined")),
    outcome(() => parsed('<svg is="x-y"></svg>').matches(":defined")),
    outcome(() => document.createElement("div", { is: "x-y" }).matches(":defined")),
    outcome(() => document.createElement("div", { is: "" }).matches(":defined")),
    outcome(() => document.createElement("div", { is: undefined }).matches(":defined")),
    outcome(() => document.createElement("div", "x-y").matches(":defined")),
    outcome(() => document.createElement("div", {}).matches(":defined")),
    outcome(() => document.createElement("div", { is: "x-y" }).cloneNode().matches(":defined")),
    outcome(() => document.createElementNS(html, "div", { is: "x-y" }).matches(":defined")),
    outcome(() => document.createElementNS("http://www.w3.org/2000/svg", "g", { is: "x-y" }).matches(":defined")),
    outcome(() => document.createElement("x-y").matches(":defined")),
    outcome(() => {
      const element = document.createElement("div", { is: "x-y" });
      return [element.getAttribute("is"), element.outerHTML];
    }),
  ];

  // The collections, live, the same object while kept.
  const collections = [];
  const tags = root.getElementsByTagName("span");
  collections.push(tags.length, tags === root.getElementsByTagName("span"), tags === root.getElementsByTagName("SPAN"));
  collections.push(root.getElementsByTagName("SPAN").length, root.getElementsByTagName("foreignObject").length);
  collections.push(root.getElementsByTagName("foreignobject").length, root.getElementsByTagName("*").length);
  collections.push(root.getElementsByTagNameNS("http://www.w3.org/2000/svg", "*").length);
  collections.push(root.getElementsByTagNameNS("*", "a").length, root.getElementsByTagNameNS("", "a").length);
  collections.push(root.getElementsByTagNameNS(null, "a").length);
  const classes = root.getElementsByClassName("a");
  collections.push(classes.length, classes === root.getElementsByClassName("a"));
  collections.push(root.getElementsByClassName(" a  B ").length, root.getElementsByClassName("").length);
  collections.push(root.getElementsByClassName("b a").length);
  const named = document.getElementsByName("r");
  collections.push(named.length, named === document.getElementsByName("r"), Object.prototype.toString.call(named));
  const span = document.createElement("span");
  root.querySelector("#d1").append(span);
  collections.push(tags.length);
  span.className = "a";
  collections.push(classes.length);
  span.remove();
  collections.push(tags.length, classes.length);
  const all = root.querySelectorAll("li");
  root.querySelector("ul").append(document.createElement("li"));
  collections.push(all.length, root.querySelectorAll("li").length, Object.prototype.toString.call(all));
  collections.push(all[0] === root.querySelector("li"), all.item(99), typeof all.forEach, [...all].length);
  collections.push(Object.keys(classes).length, classes.namedItem("p1") === p1, classes.p1 === p1);
  record.collections = collections;

  // document.all's names.
  const named_all = document.all;
  record["document.all"] = [
    outcome(() => named_all.n && named_all.n.id),
    outcome(() => named_all.p1 && named_all.p1.id),
    outcome(() => named_all.namedItem("r") && Object.prototype.toString.call(named_all.namedItem("r"))),
    outcome(() => named_all.namedItem("nothing")),
    outcome(() => named_all.item("p2") && named_all.item("p2").id),
    outcome(() => named_all.item(0).nodeName),
    outcome(() => named_all("p3") && named_all("p3").id),
    outcome(() => "p1" in named_all),
    outcome(() => "r" in named_all),
    outcome(() => named_all.r && named_all.r.length),
  ];

  // Range.
  const ranges = [];
  const text = fixture("<p>abcdef<b>ghi</b>jkl</p><p>mno</p>");
  const first = text.firstChild.firstChild;
  const range = document.createRange();
  ranges.push(describeRange(range), range.collapsed, range.commonAncestorContainer.nodeName);
  range.setStart(first, 2);
  range.setEnd(text.lastChild.firstChild, 1);
  ranges.push(describeRange(range), String(range), range.commonAncestorContainer === text);
  ranges.push(markupOf(range.cloneContents()));
  const copy = range.cloneRange();
  ranges.push(outcome(() => range.compareBoundaryPoints(Range.START_TO_END, copy)));
  ranges.push(outcome(() => range.compareBoundaryPoints(9, copy)));
  ranges.push(outcome(() => range.comparePoint(first, 1)), outcome(() => range.comparePoint(first, 4)));
  ranges.push(outcome(() => range.comparePoint(document.createElement("x"), 0)));
  ranges.push(outcome(() => range.isPointInRange(first, 3)), outcome(() => range.isPointInRange(first, 99)));
  ranges.push(outcome(() => range.intersectsNode(text.firstChild.childNodes[1])));
  ranges.push(outcome(() => range.setStart(first, 99)), outcome(() => range.setStart(document.doctype, 0)));
  ranges.push(outcome(() => range.setStartBefore(document)), outcome(() => range.selectNode(document)));
  ranges.push(outcome(() => range.setEndAfter(document.createElement("x"))));
  first.insertData(0, "XY");
  ranges.push(describeRange(range));
  first.deleteData(0, 5);
  ranges.push(describeRange(range));
  first.splitText(1);
  ranges.push(describeRange(range), String(range));
  text.firstChild.normalize();
  ranges.push(describeRange(range), String(range));
  text.firstChild.insertBefore(document.createElement("i"), text.firstChild.firstChild);
  ranges.push(describeRange(range));
  const extracted = range.extractContents();
  ranges.push(markupOf(extracted), text.innerHTML, describeRange(range), range.collapsed);
  const inserted = document.createElement("u");
  inserted.textContent = "new";
  range.insertNode(inserted);
  ranges.push(text.innerHTML, describeRange(range));
  range.selectNodeContents(inserted);
  ranges.push(describeRange(range));
  range.surroundContents(document.createElement("em"));
  ranges.push(text.innerHTML, describeRange(range));
  range.setStart(text.firstChild, 0);
  range.setEnd(text.lastChild, 1);
  ranges.push(outcome(() => range.surroundContents(document.createElement("s"))));
  range.deleteContents();
  ranges.push(text.innerHTML, describeRange(range));
  inserted.remove();
  ranges.push(describeRange(range));
  const detached = document.createElement("div");
  detached.innerHTML = "<b>1</b><b>2</b>";
  const moved = new Range();
  moved.setStart(detached, 1);
  ranges.push(point(moved.startContainer, moved.startOffset), moved.endContainer === detached);
  moved.setEnd(document.body, 0);
  ranges.push(moved.startContainer === document.body, moved.collapsed);
  ranges.push(outcome(() => markupOf(moved.createContextualFragment("<html><head><title>t</title></head><body><i>a</i></body></html>"))));
  ranges.push(outcome(() => moved.createContextualFragment("<td>c</td>").childNodes.length));
  ranges.push(outcome(() => new Range().toString()), outcome(() => Range.prototype.toString.call(moved)));
  ranges.push(outcome(() => { const r = new Range(); r.selectNode(text.lastChild); return r.toString(); }));
  ranges.push(outcome(() => new Range().insertNode(document)));
  ranges.push(outcome(() => { const r = new Range(); r.setStart(document.createComment("c"), 0); r.insertNode(document.createElement("a")); }));
  ranges.push(outcome(() => { const r = new Range(); r.setStart(document.createTextNode("tt"), 1); r.insertNode(document.createElement("a")); }));
  ranges.push(outcome(() => { const r = new Range(); r.selectNodeContents(document.doctype); }));
  ranges.push(outcome(() => { const r = new Range(); r.selectNodeContents(document); return r.cloneContents(); }));
  ranges.push(outcome(() => { const r = new Range(); r.selectNodeContents(document); return r.extractContents(); }));
  const statics = [];
  statics.push(outcome(() => { const s = new StaticRange({startContainer: first, startOffset: 9, endContainer: document, endOffset: 3}); return [s.startOffset, s.endOffset, s.collapsed, s.startContainer === first]; }));
  statics.push(outcome(() => new StaticRange({startContainer: document.doctype, startOffset: 0, endContainer: document, endOffset: 0})));
  statics.push(outcome(() => new StaticRange({startContainer: document, startOffset: 0})));
  statics.push(outcome(() => new StaticRange()));
  statics.push(outcome(() => new StaticRange({startContainer: document, startOffset: 1, endContainer: document, endOffset: 1}).collapsed));
  ranges.push(statics);
  record.ranges = ranges;

  // TreeWalker and NodeIterator.
  const walking = [];
  const tree = fixture("<div><p>a<b>b</b></p><!--c--><i>d</i><span><em>e</em></span></div>");
  const top = tree.firstChild;
  const walk = (walker, step, count = 10) => {
    const seen = [];
    for (let i = 0; i < count; ++i) {
      const node = walker[step]();
      if (!node) break;
      seen.push(node.nodeName);
    }
    return seen.join(",");
  };
  walking.push(walk(document.createTreeWalker(top), "nextNode", 20));
  walking.push(walk(document.createTreeWalker(top, NodeFilter.SHOW_ELEMENT), "nextNode"));
  walking.push(walk(document.createTreeWalker(top, NodeFilter.SHOW_TEXT | NodeFilter.SHOW_COMMENT), "nextNode"));
  const skipper = document.createTreeWalker(top, NodeFilter.SHOW_ELEMENT, (node) =>
    node.nodeName === "P" ? NodeFilter.FILTER_SKIP : node.nodeName === "SPAN" ? NodeFilter.FILTER_REJECT : NodeFilter.FILTER_ACCEPT);
  walking.push(walk(skipper, "nextNode"));
  skipper.currentNode = top.lastChild.firstChild;
  walking.push(walk(skipper, "previousNode"), walk(skipper, "parentNode"));
  skipper.currentNode = top;
  walking.push(walk(skipper, "firstChild", 1), walk(skipper, "nextSibling"), walk(skipper, "previousSibling"));
  skipper.currentNode = top;
  walking.push(walk(skipper, "lastChild", 1));
  const object_filter = { acceptNode(node) { return this === object_filter && node.nodeType === 1 ? 1 : 3; } };
  const by_object = document.createTreeWalker(top, NodeFilter.SHOW_ALL, object_filter);
  walking.push(walk(by_object, "nextNode"), by_object.filter === object_filter, by_object.root === top);
  walking.push(by_object.whatToShow, document.createTreeWalker(top).whatToShow, document.createTreeWalker(top).filter);
  walking.push(outcome(() => document.createTreeWalker(top, 1, { acceptNode: 1 }).nextNode()));
  walking.push(outcome(() => document.createTreeWalker(top, 1, {}).nextNode()));
  walking.push(outcome(() => document.createTreeWalker(top, 1, () => { throw new RangeError("mine"); }).nextNode()));
  const recursive = document.createTreeWalker(top, 1, () => recursive.nextNode());
  walking.push(outcome(() => recursive.nextNode()));
  walking.push(outcome(() => document.createTreeWalker(top, 1, () => "2").nextNode()));
  walking.push(outcome(() => document.createTreeWalker(top, 1, () => 65537).nextNode().nodeName));
  walking.push(outcome(() => document.createTreeWalker(top, 1, 5)));
  walking.push(outcome(() => document.createTreeWalker(null)));
  walking.push(outcome(() => { const w = document.createTreeWalker(top); w.currentNode = null; }));
  const iterator = document.createNodeIterator(top);
  walking.push(walk(iterator, "nextNode", 3), iterator.referenceNode.nodeName, iterator.pointerBeforeReferenceNode);
  walking.push(walk(iterator, "previousNode", 2), iterator.referenceNode.nodeName, iterator.pointerBeforeReferenceNode);
  walk(iterator, "nextNode", 2);
  const reference = iterator.referenceNode;
  reference.parentNode.removeChild(reference);
  walking.push(iterator.referenceNode.nodeName, iterator.pointerBeforeReferenceNode, walk(iterator, "nextNode"));
  const tail = document.createNodeIterator(top, NodeFilter.SHOW_ELEMENT);
  walk(tail, "nextNode", 20);
  top.lastChild.remove();
  walking.push(tail.referenceNode.nodeName, tail.pointerBeforeReferenceNode, walk(tail, "previousNode"));
  walking.push(outcome(() => document.createNodeIterator(top, 1, () => { throw new Error("x"); }).nextNode()));
  // An iterator and a range whose tree moves to another document.
  const elsewhere = new Document();
  const moving = document.createElement("div");
  moving.innerHTML = "<b>1</b><i>2</i><u>3</u>";
  const moved_iterator = document.createNodeIterator(moving, NodeFilter.SHOW_ELEMENT);
  walk(moved_iterator, "nextNode", 2);
  const moved_range = new Range();
  moved_range.selectNodeContents(moving);
  elsewhere.append(moving);
  moving.firstChild.remove();
  walking.push(moved_iterator.referenceNode.nodeName, moved_iterator.pointerBeforeReferenceNode,
    walk(moved_iterator, "nextNode"), moved_range.endOffset, moved_range.startContainer === moving);
  walking.push(NodeFilter.SHOW_ALL, NodeFilter.FILTER_SKIP, typeof NodeFilter, Object.getOwnPropertyNames(NodeFilter).length);
  record.walking = walking;

  return JSON.stringify(record);
})();
