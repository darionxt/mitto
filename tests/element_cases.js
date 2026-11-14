// What the DOM's elements, namespaces and document fragments give and throw,
// which tests/test_page_elements.py runs in the Page program and compares with
// what Chrome 157 gave for the same script, in tests/data/element_cases.json.
// Its value is the record as JSON, each case's value or error by name.

(() => {
  const SVG = "http://www.w3.org/2000/svg";
  const HTML = "http://www.w3.org/1999/xhtml";
  const MATHML = "http://www.w3.org/1998/Math/MathML";
  const XLINK = "http://www.w3.org/1999/xlink";
  const XML = "http://www.w3.org/XML/1998/namespace";
  const XMLNS = "http://www.w3.org/2000/xmlns/";
  const HTML_NAMES = ["Div", "IMG", "a", "abbr", "acronym", "address", "annotation-xml", "applet", "area", "article", "aside", "audio", "b", "base", "basefont", "bdi", "bdo", "bgsound", "big", "blink", "blockquote", "body", "br", "button", "camera", "canvas", "caption", "center", "cite", "code", "col", "colgroup", "command", "credential", "custom-el", "data", "datalist", "dd", "del", "details", "dfn", "dialog", "dir", "div", "dl", "dt", "em", "embed", "fencedframe", "fieldset", "figcaption", "figure", "font", "foo", "footer", "form", "frame", "frameset", "geolocation", "h1", "h2", "h3", "h4", "h5", "h6", "head", "header", "hgroup", "hr", "html", "i", "iframe", "image", "img", "input", "ins", "install", "isindex", "kbd", "keygen", "label", "layer", "legend", "li", "link", "listing", "login", "main", "map", "mark", "marquee", "math", "menu", "menubar", "menuitem", "menulist", "meta", "meter", "microphone", "multicol", "my-element", "nav", "nextid", "nobr", "noembed", "noframes", "nolayer", "noscript", "object", "ol", "optgroup", "option", "output", "p", "param", "persistentwidget", "picture", "plaintext", "pre", "progress", "q", "rb", "rp", "rt", "rtc", "ruby", "s", "samp", "script", "search", "section", "select", "selectedcontent", "selectmenu", "slot", "small", "source", "spacer", "span", "strike", "strong", "style", "sub", "submenu", "summary", "sup", "svg", "table", "tbody", "td", "template", "textarea", "tfoot", "th", "thead", "time", "title", "tr", "track", "tt", "u", "ul", "usermedia", "var", "video", "wbr", "xmp"];
  const show = (value) => {
    if (value === null) return "null";
    if (value === undefined) return "undefined";
    if (typeof value === "object" || typeof value === "function") return Object.prototype.toString.call(value);
    return typeof value + ":" + String(value);
  };
  const cases = {};
  const run = (name, f) => {
    try {
      cases[name] = show(f());
    } catch (e) {
      cases[name] = "threw " + e.name + ": " + e.message;
    }
  };
  const svgNames = ["a","animate","animateMotion","animateTransform","set","circle","clipPath","defs","desc","ellipse","feBlend","feColorMatrix","feComponentTransfer","feComposite","feConvolveMatrix","feDiffuseLighting","feDisplacementMap","feDistantLight","feDropShadow","feFlood","feFuncA","feFuncB","feFuncG","feFuncR","feGaussianBlur","feImage","feMerge","feMergeNode","feMorphology","feOffset","fePointLight","feSpecularLighting","feSpotLight","feTile","feTurbulence","filter","foreignObject","g","image","line","linearGradient","marker","mask","metadata","mpath","path","pattern","polygon","polyline","radialGradient","rect","script","stop","style","svg","switch","symbol","text","textPath","title","tspan","use","view","animateColor","unknown","RECT","foreignobject","hkern","font"];
  for (const name of svgNames) run("svg " + name, () => document.createElementNS(SVG, name));
  for (const name of ["math","mi","mo","mn","ms","mtext","mrow","msqrt","mroot","mfrac","msub","msup","msubsup","munder","mover","munderover","mmultiscripts","mtable","mtr","mtd","mspace","mpadded","mphantom","mstyle","merror","semantics","annotation","annotation-xml","a","menclose","none","mprescripts","foo"]) {
    run("mathml " + name, () => document.createElementNS(MATHML, name));
  }
  run("svg rect parts", () => { const e = document.createElementNS(SVG, "rect"); return [e.namespaceURI, e.prefix, e.localName, e.tagName, e.nodeName].join(); });
  run("svg prefixed", () => { const e = document.createElementNS(SVG, "s:rect"); return [e, e.prefix, e.localName, e.tagName, e.outerHTML].map(String).join(); });
  run("html prefixed", () => { const e = document.createElementNS(HTML, "x:div"); return [e, e.prefix, e.localName, e.tagName, e.outerHTML].map(String).join(); });
  run("html upper", () => { const e = document.createElementNS(HTML, "DIV"); return [e, e.localName, e.tagName].map(String).join(); });
  run("html template ns", () => document.createElementNS(HTML, "template"));
  run("null ns", () => { const e = document.createElementNS(null, "div"); return [e, e.namespaceURI, e.localName, e.tagName].map(String).join(); });
  run("empty ns", () => document.createElementNS("", "div").namespaceURI);
  run("other ns", () => { const e = document.createElementNS("urn:x", "p:Foo"); return [e, e.namespaceURI, e.prefix, e.localName, e.tagName, e.outerHTML].map(String).join(); });
  run("two colons", () => { const e = document.createElementNS("urn:x", "a:b:c"); return [e.prefix, e.localName, e.tagName].join(); });
  const bad = [[null, "a:b"], ["", "a:b"], ["urn:x", "xml:lang"], [XML, "xml:lang"], [XMLNS, "foo"], [XMLNS, "xmlns"], [XMLNS, "xmlns:a"], [null, "xmlns"], ["urn:x", "xmlns:a"], [SVG, ""], [SVG, ":a"], [SVG, "a:"], [SVG, "1a"], [SVG, "a b"], [SVG, "a>b"], [SVG, "a/b"], [SVG, "p>:b"], [SVG, "-a"], [SVG, "éa"], [SVG, "_a"], [SVG, ":"], [SVG, "a::b"]];
  for (const [ns, qn] of bad) run(`createElementNS ${ns} ${JSON.stringify(qn)}`, () => { const e = document.createElementNS(ns, qn); return [e, e.prefix, e.localName].map(String).join(); });
  run("createElementNS options", () => document.createElementNS(SVG, "rect", {}));
  run("createElementNS options bad", () => document.createElementNS(SVG, "a b", {}));
  run("createElementNS options two colons", () => document.createElementNS("urn:x", "a:b:c", {}).localName);
  run("createElementNS options digit after", () => document.createElementNS("urn:x", "a:1", {}));
  run("createElementNS no args", () => document.createElementNS());
  run("createElementNS one arg", () => document.createElementNS(SVG));
  // Attributes with namespaces.
  run("setAttributeNS xlink", () => {
    const e = document.createElementNS(SVG, "use");
    e.setAttributeNS(XLINK, "xlink:href", "#a");
    e.setAttributeNS(XML, "xml:lang", "en");
    e.setAttributeNS(XMLNS, "xmlns:xlink", XLINK);
    e.setAttributeNS(XMLNS, "xmlns", SVG);
    e.setAttributeNS("urn:x", "p:q", "1");
    e.setAttributeNS("urn:y", "r", "2");
    e.setAttributeNS(null, "plain", "3");
    return [e.getAttributeNames().join(" "), e.outerHTML, e.getAttribute("xlink:href"), e.getAttributeNS(XLINK, "href"), e.getAttributeNS(XLINK, "xlink:href"), e.hasAttributeNS(XLINK, "href"), e.getAttribute("href"), e.getAttribute("r"), e.hasAttribute("p:q"), e.getAttributeNS("", "plain"), e.getAttributeNS(null, "plain")].map(String).join("|");
  });
  run("setAttributeNS keeps prefix", () => {
    const e = document.createElementNS(SVG, "use");
    e.setAttributeNS(XLINK, "xlink:href", "#a");
    e.setAttributeNS(XLINK, "other:href", "#b");
    return [e.getAttributeNames().join(" "), e.getAttributeNS(XLINK, "href"), e.getAttributeNames().length].join("|");
  });
  run("setAttribute after NS", () => {
    const e = document.createElement("div");
    e.setAttributeNS("urn:x", "foo", "1");
    e.setAttribute("foo", "2");
    e.setAttribute("FOO", "3");
    return [e.getAttributeNames().join(" "), e.getAttributeNS("urn:x", "foo"), e.getAttribute("foo"), e.outerHTML].join("|");
  });
  run("getAttribute case", () => {
    const e = document.createElement("div");
    e.setAttributeNS("urn:x", "P:Foo", "1");
    e.setAttributeNS("urn:x", "Bar", "2");
    return [e.getAttribute("P:Foo"), e.getAttribute("p:foo"), e.getAttribute("Bar"), e.getAttribute("bar"), e.hasAttribute("Bar"), e.outerHTML].map(String).join("|");
  });
  run("svg getAttribute case", () => {
    const e = document.createElementNS(SVG, "svg");
    e.setAttribute("viewBox", "0 0 1 1");
    e.setAttribute("VIEWBOX", "x");
    return [e.getAttributeNames().join(" "), e.getAttribute("viewbox"), e.getAttribute("viewBox")].map(String).join("|");
  });
  run("removeAttributeNS", () => {
    const e = document.createElement("div");
    e.setAttributeNS("urn:x", "p:a", "1");
    e.setAttributeNS("urn:y", "p:a", "2");
    e.setAttribute("a", "3");
    e.removeAttributeNS("urn:x", "a");
    const after1 = e.getAttributeNames().join(" ");
    e.removeAttributeNS(null, "a");
    const after2 = e.getAttributeNames().join(" ");
    e.removeAttribute("p:a");
    return [after1, after2, e.getAttributeNames().join(" "), e.getAttributeNS("urn:y", "a")].map(String).join("|");
  });
  run("toggleAttribute prefixed", () => {
    const e = document.createElement("div");
    e.setAttributeNS("urn:x", "p:a", "1");
    return [e.toggleAttribute("p:a"), e.getAttributeNames().join(" "), e.toggleAttribute("p:a"), e.getAttributeNames().join(" ")].join("|");
  });
  const badAttr = [[null, "a:b"], ["urn:x", "xmlns"], ["urn:x", "xmlns:a"], [XMLNS, "a"], [XMLNS, "a:b"], ["urn:x", "xml:a"], [null, ""], [null, "a b"], [null, "a=b"], [null, ":a"], [null, "a:"], [null, "1a"], [XLINK, "xlink:a/b"]];
  for (const [ns, qn] of badAttr) run(`setAttributeNS ${ns} ${JSON.stringify(qn)}`, () => { const e = document.createElement("div"); e.setAttributeNS(ns, qn, "v"); return e.getAttributeNames().join(" "); });
  run("setAttributeNS no args", () => document.createElement("div").setAttributeNS());
  run("setAttributeNS two args", () => document.createElement("div").setAttributeNS(null, "a"));
  run("getAttributeNS one arg", () => document.createElement("div").getAttributeNS(null));
  run("hasAttributeNS one arg", () => document.createElement("div").hasAttributeNS(null));
  run("removeAttributeNS one arg", () => document.createElement("div").removeAttributeNS(null));
  // Namespace lookups.
  run("lookups", () => {
    const root = document.createElementNS(SVG, "svg");
    root.setAttributeNS(XMLNS, "xmlns:xl", XLINK);
    root.setAttributeNS(XMLNS, "xmlns:empty", "");
    const child = document.createElementNS("urn:c", "c:child");
    root.appendChild(child);
    const text = document.createTextNode("t");
    child.appendChild(text);
    const frag = document.createDocumentFragment();
    return [
      root.lookupNamespaceURI(null), root.lookupNamespaceURI(""), root.lookupNamespaceURI("xl"), root.lookupNamespaceURI("empty"),
      root.lookupNamespaceURI("xml"), root.lookupNamespaceURI("xmlns"), root.lookupNamespaceURI("nope"),
      child.lookupNamespaceURI("c"), child.lookupNamespaceURI(null), child.lookupNamespaceURI("xl"), text.lookupNamespaceURI("c"),
      root.lookupPrefix(XLINK), root.lookupPrefix(SVG), child.lookupPrefix("urn:c"), child.lookupPrefix(XLINK), text.lookupPrefix("urn:c"),
      root.lookupPrefix(null), root.lookupPrefix(""), frag.lookupPrefix(XLINK), frag.lookupNamespaceURI(null),
      root.isDefaultNamespace(SVG), root.isDefaultNamespace(null), child.isDefaultNamespace(SVG), child.isDefaultNamespace(""),
      document.lookupNamespaceURI(null), document.isDefaultNamespace(HTML), document.lookupPrefix(HTML), document.doctype && document.doctype.lookupNamespaceURI(null),
      document.body.lookupNamespaceURI(null), document.createElement("p").lookupNamespaceURI(null), document.createElement("p").lookupPrefix(HTML),
    ].map(String).join("|");
  });
  run("lookup xmlns attr", () => {
    const e = document.createElement("div");
    e.setAttributeNS(XMLNS, "xmlns", "urn:default");
    e.setAttributeNS(XMLNS, "xmlns:p", "urn:p");
    return [e.lookupNamespaceURI(null), e.lookupNamespaceURI("p"), e.lookupPrefix("urn:p"), e.isDefaultNamespace(HTML)].map(String).join("|");
  });
  run("lookupPrefix no args", () => document.body.lookupPrefix());
  run("lookupNamespaceURI no args", () => document.body.lookupNamespaceURI());
  run("isDefaultNamespace no args", () => document.body.isDefaultNamespace());
  // Equality with namespaces.
  run("isEqualNode ns", () => {
    const a = document.createElementNS(SVG, "use");
    const b = document.createElementNS(SVG, "use");
    a.setAttributeNS(XLINK, "xlink:href", "#x");
    b.setAttributeNS(XLINK, "l:href", "#x");
    const c = document.createElementNS(SVG, "use");
    c.setAttribute("href", "#x");
    return [a.isEqualNode(b), a.isEqualNode(c), a.cloneNode().isEqualNode(a), a.cloneNode().getAttributeNames().join()].join("|");
  });
  run("clone prefix", () => { const e = document.createElementNS(HTML, "x:p").cloneNode(); return [e, e.prefix, e.tagName].map(String).join(); });
  // DocumentFragment.
  run("fragment basics", () => {
    const f = new DocumentFragment();
    return [f, f.nodeType, f.nodeName, f.nodeValue, f.ownerDocument === document, f.parentNode, f.textContent, f.childElementCount, f.isConnected].map(String).join("|");
  });
  run("createDocumentFragment", () => document.createDocumentFragment());
  run("fragment insert", () => {
    const f = document.createDocumentFragment();
    f.append("a", document.createElement("b"), "c");
    f.firstElementChild.id = "x";
    const div = document.createElement("div");
    div.appendChild(document.createElement("i"));
    const found = f.getElementById("x");
    const returned = div.insertBefore(f, div.firstChild);
    return [returned === f, f.childNodes.length, div.innerHTML, found && found.parentNode === div, f.textContent, f.getElementById("x")].map(String).join("|");
  });
  run("fragment textContent", () => {
    const f = document.createDocumentFragment();
    f.append("a", document.createElement("b"));
    f.lastChild.textContent = "b";
    const before = f.textContent;
    f.textContent = "z";
    return [before, f.childNodes.length, f.firstChild.data].join("|");
  });
  run("fragment into document", () => {
    const doc = document.implementation ? new Document() : null;
    const f = doc.createDocumentFragment();
    f.append(doc.createElement("a"), doc.createElement("b"));
    doc.appendChild(f);
    return doc.childNodes.length;
  });
  run("fragment text into document", () => {
    const doc = new Document();
    const f = doc.createDocumentFragment();
    f.append("t");
    doc.appendChild(f);
  });
  run("fragment replaceChild", () => {
    const div = document.createElement("div");
    div.innerHTML = "<i></i><b></b>";
    const f = document.createDocumentFragment();
    f.append("1", "2");
    div.replaceChild(f, div.firstChild);
    return [div.innerHTML, div.childNodes.length].join("|");
  });
  run("fragment in append", () => {
    const div = document.createElement("div");
    const f = document.createDocumentFragment();
    f.append(document.createElement("i"), "t");
    div.append("x", f, "y");
    return [div.innerHTML, f.childNodes.length].join("|");
  });
  run("fragment contains parent", () => {
    const f = document.createDocumentFragment();
    const div = document.createElement("div");
    f.appendChild(div);
    div.appendChild(f);
  });
  run("fragment clone", () => {
    const f = document.createDocumentFragment();
    f.append(document.createElement("i"), "t");
    const c = f.cloneNode(true);
    return [c, c.childNodes.length, c.firstChild !== f.firstChild, f.cloneNode().childNodes.length, f.isEqualNode(c)].map(String).join("|");
  });
  run("fragment querySelector", () => {
    const f = document.createDocumentFragment();
    f.append(document.createElement("i"), document.createElement("b"));
    return [f.querySelector("b"), f.children.length, f.firstElementChild.localName, f.lastElementChild.localName].map(String).join("|");
  });
  run("fragment adopted", () => {
    const doc = new Document();
    const f = doc.createDocumentFragment();
    f.append(doc.createElement("x"));
    document.body.appendChild(f);
    const x = document.body.lastChild;
    const r = [x.ownerDocument === document, f.ownerDocument === doc].join("|");
    x.remove();
    return r;
  });
  run("fragment alone empties before checks", () => {
    // Blink's ConvertNodeUnionsIntoNodes() takes a fragment's children out before the insertion is checked.
    const doc = new Document();
    doc.appendChild(doc.createElement("r"));
    const f = doc.createDocumentFragment();
    f.append(doc.createElement("i"));
    let thrown = "none";
    try { doc.append(f); } catch (e) { thrown = e.name; }
    let prepended = "none";
    const g = doc.createDocumentFragment();
    g.append(doc.createElement("b"));
    try { doc.prepend(g); } catch (e) { prepended = e.name; }
    return [thrown, f.childNodes.length, prepended, g.childNodes.length, doc.childNodes.length].join("|");
  });
  run("fragment appended to itself", () => {
    const f = document.createDocumentFragment();
    f.append(document.createElement("b"), "t");
    f.append(f);
    return [f.childNodes.length, f.firstChild.localName].join("|");
  });
  // Templates.
  run("template content", () => {
    const t = document.createElement("template");
    const c = t.content;
    return [c, c === t.content, c.ownerDocument === document, c.ownerDocument && c.ownerDocument.defaultView, c.ownerDocument instanceof HTMLDocument, c.ownerDocument.URL, c.ownerDocument.compatMode, c.childNodes.length, t.childNodes.length, c.parentNode].map(String).join("|");
  });
  run("template document shared", () => {
    const a = document.createElement("template");
    const b = document.createElement("template");
    const inner = a.content.ownerDocument.createElement("template");
    return [a.content.ownerDocument === b.content.ownerDocument, inner.content.ownerDocument === a.content.ownerDocument].join("|");
  });
  run("template innerHTML", () => {
    const t = document.createElement("template");
    t.innerHTML = "<p>a</p><b>c</b>";
    return [t.childNodes.length, t.content.childNodes.length, t.innerHTML, t.outerHTML, t.content.firstChild.ownerDocument === t.content.ownerDocument, t.textContent].map(String).join("|");
  });
  run("template clone", () => {
    const t = document.createElement("template");
    t.innerHTML = "<p>a</p>";
    t.setAttribute("id", "t");
    const deep = t.cloneNode(true);
    const shallow = t.cloneNode(false);
    return [deep.content.childNodes.length, deep.content.firstChild !== t.content.firstChild, shallow.content.childNodes.length, deep.innerHTML, t.isEqualNode(deep), deep.content.ownerDocument === t.content.ownerDocument].map(String).join("|");
  });
  run("template adopt", () => {
    const doc = new Document();
    const t = document.createElement("template");
    t.content.appendChild(document.createElement("p"));
    const before = t.content.ownerDocument;
    doc.appendChild(t);
    return [t.ownerDocument === doc, t.content.ownerDocument !== before, t.content.ownerDocument === doc, t.content.firstChild.ownerDocument === t.content.ownerDocument, t.content.ownerDocument instanceof HTMLDocument].join("|");
  });
  run("template contents hold no template ancestor", () => {
    const t = document.createElement("template");
    let own = "none";
    try { t.content.appendChild(t); } catch (e) { own = e.name; }
    const t2 = document.createElement("template");
    const d = document.createElement("div");
    d.appendChild(t2);
    let outer = "none";
    try { t2.content.append(d); } catch (e) { outer = e.name + ": " + e.message; }
    return [own, outer, t.content.childNodes.length].join("|");
  });
  run("outerHTML in a fragment", () => {
    const f = document.createDocumentFragment();
    const e = document.createElement("i");
    f.appendChild(e);
    e.outerHTML = "<b>x</b>y<p>z</p>";
    const t = document.createElement("template");
    t.innerHTML = "<p>a</p>";
    t.content.firstChild.outerHTML = "<em>z</em>w";
    return [[...f.childNodes].map((n) => n.nodeName).join(","), t.innerHTML].join("|");
  });
  run("template content setter", () => { const t = document.createElement("template"); t.content = 1; return t.content; });
  run("template shadowRootMode", () => { const t = document.createElement("template"); const a = t.shadowRootMode; t.setAttribute("shadowrootmode", "OPEN"); const b = t.shadowRootMode; t.shadowRootMode = "x"; return [a, b, t.getAttribute("shadowrootmode"), t.shadowRootMode].join("|"); });
  run("svg in html serialize", () => {
    const div = document.createElement("div");
    const svg = document.createElementNS(SVG, "svg");
    const fo = document.createElementNS(SVG, "foreignObject");
    fo.setAttribute("requiredExtensions", "x");
    svg.appendChild(fo);
    const m = document.createElementNS(MATHML, "math");
    m.setAttributeNS(XLINK, "xlink:href", "u");
    div.append(svg, m);
    return div.innerHTML;
  });
  run("svg instanceof", () => { const e = document.createElementNS(SVG, "circle"); return [e instanceof SVGGeometryElement, e instanceof SVGGraphicsElement, e instanceof SVGElement, e instanceof Element, e instanceof HTMLElement].join(); });
  run("svg getElementById", () => { const e = document.createElementNS(SVG, "circle"); e.id = "c"; return [e.id, e.getAttribute("id")].join(); });
  run("svg className", () => { const e = document.createElementNS(SVG, "circle"); return Object.prototype.toString.call(e.className); });
  run("mathml parts", () => { const e = document.createElementNS(MATHML, "mi"); return [e.namespaceURI, e.tagName, e instanceof MathMLElement, e instanceof HTMLElement].join(); });
  run("html in svg ns names", () => document.createElementNS(SVG, "div"));
  // Reflected attributes of every kind. URLs resolve in a document of about:blank, whose base is the same in Chrome's
  // case page and in the Page.
  const blank = new Document();
  for (const value of [null, "", "http://example.com/a b", "  http://example.com/x?y#z \n", "http://[bad", "//host/path", "x/y", "?q", "#f", "javascript:alert(1)", "\u00e9", "http://EXAMPLE.com:80/%7e", "data:,x", "about:blank?x"]) {
    run(`link href ${JSON.stringify(value)}`, () => {
      const e = blank.createElementNS(HTML, "link");
      if (value !== null) e.setAttribute("href", value);
      return e.href;
    });
  }
  run("link href setter", () => { const e = blank.createElementNS(HTML, "link"); e.href = "  #rel "; return [e.getAttribute("href"), e.href].join("|"); });
  run("link href absolute in page", () => { const e = document.createElement("link"); e.href = "https://example.com/../a/./b?c#d"; return e.href; });
  run("link href no arg", () => { const e = document.createElement("link"); const d = Object.getOwnPropertyDescriptor(HTMLLinkElement.prototype, "href"); d.set.call(e); return e.getAttribute("href"); });
  run("link href surrogate", () => { const e = document.createElement("link"); e.href = "http://a/\ud800"; return e.getAttribute("href").charCodeAt(9); });
  for (const value of [null, "12", "  12abc", "-3", "+4", "2147483647", "2147483648", "-2147483648", "-2147483649", "abc", "", " ", "0x10", "1e3", " 12", "\t-0"]) {
    run(`li value ${JSON.stringify(value)}`, () => {
      const e = document.createElement("li");
      if (value !== null) e.setAttribute("value", value);
      return e.value;
    });
  }
  for (const value of [-5, 3.9, -3.9, 2 ** 31, 2 ** 32 + 7, NaN, "8", null, true]) {
    run(`li value set ${String(value)}`, () => { const e = document.createElement("li"); e.value = value; return e.getAttribute("value"); });
  }
  run("pre width", () => { const e = document.createElement("pre"); e.setAttribute("width", " 80px"); const a = e.width; e.width = -1; return [a, e.getAttribute("width")].join("|"); });
  run("body bgColor null", () => { const e = document.createElement("body"); e.bgColor = null; e.text = null; e.link = undefined; return [e.getAttribute("bgcolor"), e.getAttribute("text"), e.getAttribute("link"), e.bgColor, e.vLink].map(String).join("|"); });
  run("aria label", () => {
    const e = document.createElement("div");
    const before = e.ariaLabel;
    e.ariaLabel = "x";
    const set = e.getAttribute("aria-label");
    e.ariaLabel = null;
    const removed = e.hasAttribute("aria-label");
    e.ariaLabel = undefined;
    const undef = e.hasAttribute("aria-label");
    e.setAttribute("aria-label", "y");
    return [before, set, removed, undef, e.ariaLabel].map(String).join("|");
  });
  run("aria on svg", () => { const e = document.createElementNS(SVG, "rect"); e.ariaHidden = "true"; return [e.getAttribute("aria-hidden"), e.ariaHidden, e.ariaRowIndexText].map(String).join("|"); });
  run("aria names", () => { const e = document.createElement("div"); e.ariaColIndex = "1"; e.ariaRoleDescription = "r"; e.ariaKeyShortcuts = "k"; e.role = "button"; return e.getAttributeNames().join(" "); });
  run("aria no arg", () => { const d = Object.getOwnPropertyDescriptor(Element.prototype, "ariaLabel"); d.set.call(document.createElement("div")); });
  run("meta name no arg", () => { const d = Object.getOwnPropertyDescriptor(HTMLMetaElement.prototype, "name"); d.set.call(document.createElement("meta")); });
  run("script async no arg", () => { const d = Object.getOwnPropertyDescriptor(HTMLScriptElement.prototype, "noModule"); d.set.call(document.createElement("script")); });
  run("body bgColor no arg", () => { const d = Object.getOwnPropertyDescriptor(HTMLBodyElement.prototype, "bgColor"); d.set.call(document.createElement("body")); });
  run("li value no arg", () => { const d = Object.getOwnPropertyDescriptor(HTMLLIElement.prototype, "value"); const e = document.createElement("li"); d.set.call(e); return e.getAttribute("value"); });
  run("link crossOrigin no arg", () => { const d = Object.getOwnPropertyDescriptor(HTMLLinkElement.prototype, "crossOrigin"); const e = document.createElement("link"); d.set.call(e); return e.getAttribute("crossorigin"); });
  run("link fetchPriority no arg", () => { const d = Object.getOwnPropertyDescriptor(HTMLLinkElement.prototype, "fetchPriority"); const e = document.createElement("link"); d.set.call(e); return e.getAttribute("fetchpriority"); });
  // Attr nodes and NamedNodeMap.
  run("attr basics", () => {
    const e = document.createElement("div");
    e.setAttribute("Id", "x");
    const a = e.getAttributeNode("id");
    return [a, a.nodeType, a.nodeName, a.name, a.localName, a.prefix, a.namespaceURI, a.value, a.nodeValue, a.textContent, a.ownerElement === e, a.specified, a.parentNode, a.ownerDocument === document, a.isConnected, a.childNodes.length, a.firstChild, a.hasChildNodes(), a === e.getAttributeNode("ID"), a === e.attributes[0], a === e.attributes.id].map(String).join("|");
  });
  run("attr live value", () => {
    const e = document.createElement("div");
    e.setAttribute("title", "a");
    const a = e.getAttributeNode("title");
    e.setAttribute("title", "b");
    const read = a.value;
    a.value = "c";
    const back = e.getAttribute("title");
    a.textContent = null;
    const cleared = e.getAttribute("title");
    a.nodeValue = "d";
    return [read, back, JSON.stringify(cleared), e.getAttribute("title")].join("|");
  });
  run("attr detached by removal", () => {
    const e = document.createElement("div");
    e.setAttribute("title", "a");
    const a = e.getAttributeNode("title");
    e.removeAttribute("title");
    const afterRemove = [a.ownerElement, a.value].map(String).join(",");
    e.setAttribute("title", "new");
    return [afterRemove, a.value, a === e.getAttributeNode("title")].join("|");
  });
  run("createAttribute", () => {
    const a = document.createAttribute("FOO");
    const b = new Document().createAttribute("FOO");
    return [a.name, a.value, a.ownerElement, b.name, a.ownerDocument === document].map(String).join("|");
  });
  for (const name of ["", "a b", "a=b", "a>", "1a", ":a", "a:b"]) run(`createAttribute ${JSON.stringify(name)}`, () => document.createAttribute(name).name);
  run("createAttributeNS", () => {
    const a = document.createAttributeNS("http://www.w3.org/1999/xlink", "xlink:href");
    return [a.name, a.prefix, a.localName, a.namespaceURI, a.value].map(String).join("|");
  });
  for (const [ns, qn] of [[null, "a:b"], ["urn:x", "xmlns"], ["urn:x", ""], ["urn:x", "a b"]]) run(`createAttributeNS ${ns} ${JSON.stringify(qn)}`, () => document.createAttributeNS(ns, qn).name);
  run("setAttributeNode", () => {
    const e = document.createElement("div");
    e.setAttribute("title", "old");
    const before = e.getAttributeNode("title");
    const a = document.createAttribute("title");
    a.value = "new";
    const old = e.setAttributeNode(a);
    return [old === before, old.value, old.ownerElement, a.ownerElement === e, e.getAttribute("title"), e.setAttributeNode(a) === a, e.attributes.length].map(String).join("|");
  });
  run("setAttributeNode fresh old", () => {
    const e = document.createElement("div");
    e.setAttribute("title", "old");
    const a = document.createAttribute("title");
    const old = e.setAttributeNode(a);
    return [old && old.name, old && old.value, old && old.ownerElement, e.getAttribute("title")].map(String).join("|");
  });
  run("setAttributeNode none", () => { const e = document.createElement("div"); return String(e.setAttributeNode(document.createAttribute("x"))) + "|" + e.outerHTML; });
  run("setAttributeNode in use", () => {
    const e = document.createElement("div");
    e.setAttribute("title", "a");
    document.createElement("p").setAttributeNode(e.getAttributeNode("title"));
  });
  run("setAttributeNode other document", () => {
    const doc = new Document();
    const a = doc.createAttributeNS(null, "data-x");
    const e = document.createElement("div");
    e.setAttributeNode(a);
    return [a.ownerDocument === document, e.getAttribute("data-x")].join("|");
  });
  run("setAttributeNode wrong type", () => document.createElement("div").setAttributeNode(document.createElement("p")));
  run("setAttributeNodeNS", () => {
    const e = document.createElementNS("http://www.w3.org/2000/svg", "use");
    const a = document.createAttributeNS("http://www.w3.org/1999/xlink", "xlink:href");
    a.value = "#x";
    e.setAttributeNodeNS(a);
    return [e.getAttributeNS("http://www.w3.org/1999/xlink", "href"), e.outerHTML].join("|");
  });
  // Blink makes the replaced attribute's node with the new name, unless a page asked for its node before.
  run("setAttributeNodeNS other prefix", () => {
    const e = document.createElement("div");
    e.setAttributeNS("urn:x", "p:a", "old");
    const r = e.setAttributeNodeNS(document.createAttributeNS("urn:x", "q:a"));
    return [r.name, r.prefix, r.value, r.ownerElement, e.attributes[0].name].join("|");
  });
  run("setAttributeNodeNS other prefix asked", () => {
    const e = document.createElement("div");
    e.setAttributeNS("urn:x", "p:a", "old");
    e.getAttributeNodeNS("urn:x", "a");
    const r = e.setAttributeNodeNS(document.createAttributeNS("urn:x", "q:a"));
    return [r.name, r.prefix, r.value, e.attributes[0].name].join("|");
  });
  // Blink orders an attribute's node by its qualified name, which keeps the old prefix in the element's attributes.
  run("attr position other prefix", () => {
    const e = document.createElement("div");
    e.setAttributeNS("urn:x", "p:a", "1");
    e.setAttributeNS("urn:x", "p:b", "2");
    const qa = document.createAttributeNS("urn:x", "q:a");
    e.setAttributeNodeNS(qa);
    const pb = e.getAttributeNodeNS("urn:x", "b");
    return [qa.compareDocumentPosition(pb), pb.compareDocumentPosition(qa), e.getAttributeNames().join(" ")].join("|");
  });
  run("removeAttributeNode", () => {
    const e = document.createElement("div");
    e.setAttribute("title", "a");
    const a = e.getAttributeNode("title");
    const r = e.removeAttributeNode(a);
    return [r === a, a.ownerElement, a.value, e.hasAttribute("title")].map(String).join("|");
  });
  run("removeAttributeNode other", () => document.createElement("div").removeAttributeNode(document.createAttribute("x")));
  run("removeAttributeNode twice", () => { const e = document.createElement("div"); e.setAttribute("a", "1"); const a = e.getAttributeNode("a"); e.removeAttributeNode(a); e.removeAttributeNode(a); });
  run("getAttributeNodeNS", () => {
    const e = document.createElement("div");
    e.setAttributeNS("urn:x", "p:a", "1");
    return [e.getAttributeNodeNS("urn:x", "a").name, e.getAttributeNodeNS("urn:x", "p:a"), e.getAttributeNode("p:a").value, e.getAttributeNode("P:A"), e.getAttributeNode("nope")].map(String).join("|");
  });
  run("named node map", () => {
    const e = document.createElement("div");
    e.setAttribute("id", "i");
    e.setAttribute("class", "c");
    e.setAttributeNS("urn:x", "p:Up", "u");
    e.setAttributeNS("urn:y", "Low", "l");
    const m = e.attributes;
    return [m, m === e.attributes, m.length, m.item(0).name, m.item(5), m[1].name, m[9], m.getNamedItem("class").value, m.getNamedItem("CLASS").value, m.getNamedItemNS("urn:x", "Up").name, Object.keys(m).join(","), Object.getOwnPropertyNames(m).join(","), "id" in m, "p:Up" in m, m["p:Up"].value, JSON.stringify(Object.getOwnPropertyDescriptor(m, "id") && Object.keys(Object.getOwnPropertyDescriptor(m, "id")))].map(String).join("|");
  });
  run("named node map live", () => {
    const e = document.createElement("div");
    const m = e.attributes;
    const before = m.length;
    e.setAttribute("a", "1");
    return [before, m.length, m[0].value].join("|");
  });
  run("named node map remove", () => {
    const e = document.createElement("div");
    e.setAttribute("a", "1");
    e.setAttributeNS("urn:x", "p:b", "2");
    const r = e.attributes.removeNamedItem("a");
    const r2 = e.attributes.removeNamedItemNS("urn:x", "b");
    return [r.name, r.value, r.ownerElement, r2.name, e.attributes.length].map(String).join("|");
  });
  run("named node map remove missing", () => document.createElement("div").attributes.removeNamedItem("x"));
  run("named node map removeNS missing", () => document.createElement("div").attributes.removeNamedItemNS("urn:x", "y"));
  run("named node map removeNS null", () => document.createElement("div").attributes.removeNamedItemNS(null, "y"));
  run("named node map set", () => {
    const e = document.createElement("div");
    const a = document.createAttribute("x");
    a.value = "v";
    return [e.attributes.setNamedItem(a), e.getAttribute("x"), e.attributes.setNamedItemNS(document.createAttribute("y")), e.attributes.length].map(String).join("|");
  });
  run("named node map assign", () => { "use strict"; const m = document.createElement("div").attributes; m[0] = 1; });
  run("named node map iterate", () => { const e = document.createElement("div"); e.setAttribute("a", "1"); e.setAttribute("b", "2"); return [...e.attributes].map((a) => a.name).join(","); });
  run("attr clone and equal", () => {
    const e = document.createElement("div");
    e.setAttribute("title", "a");
    const a = e.getAttributeNode("title");
    const c = a.cloneNode();
    return [c.ownerElement, c.value, c.isEqualNode(a), a.isEqualNode(document.createAttribute("title")), c === a].map(String).join("|");
  });
  run("attr appendChild", () => document.createAttribute("x").appendChild(document.createTextNode("t")));
  run("attr into element", () => document.createElement("div").appendChild(document.createAttribute("x")));
  run("attr lookups", () => {
    const e = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    e.setAttribute("title", "a");
    const a = e.getAttributeNode("title");
    const free = document.createAttribute("x");
    return [a.lookupNamespaceURI(null), a.isDefaultNamespace("http://www.w3.org/2000/svg"), free.lookupNamespaceURI(null), a.baseURI === document.baseURI, a.getRootNode() === a].map(String).join("|");
  });
  run("attr positions", () => {
    const root = document.createElement("div");
    const child = document.createElement("p");
    root.appendChild(child);
    root.setAttribute("a", "1");
    root.setAttribute("b", "2");
    child.setAttribute("c", "3");
    const a = root.getAttributeNode("a");
    const b = root.getAttributeNode("b");
    const c = child.getAttributeNode("c");
    const free = document.createAttribute("f");
    const pos = (x, y) => x.compareDocumentPosition(y);
    return [pos(a, b), pos(b, a), pos(a, root), pos(root, a), pos(a, child), pos(child, a), pos(a, c), pos(c, a), pos(c, child), pos(child, c), pos(a, a), pos(a, free) & 1, pos(free, root) & 1].join(",");
  });
  run("attr textContent of element", () => { const e = document.createElement("div"); e.setAttribute("a", "1"); return e.textContent === "" && e.getAttributeNode("a").textContent; });
  run("attr move with element", () => {
    const doc = new Document();
    const e = document.createElement("div");
    e.setAttribute("a", "1");
    const a = e.getAttributeNode("a");
    doc.appendChild(e);
    return [a.ownerDocument === doc, e.ownerDocument === doc].join("|");
  });
  run("attr value no arg", () => { const d = Object.getOwnPropertyDescriptor(Attr.prototype, "value"); const a = document.createAttribute("x"); d.set.call(a); return a.value; });
  // DOMTokenList.
  run("classList basics", () => {
    const e = document.createElement("div");
    e.className = "  a b\ta  c\n";
    const l = e.classList;
    return [l, l === e.classList, l.length, l.item(0), l.item(3), l[1], l[9], l.value, String(l), l.toString(), l.contains("c"), l.contains("A"), [...l].join(","), Object.keys(l).join(","), [...l.keys()].join(","), [...l.entries()].join(";")].map(String).join("|");
  });
  run("classList add", () => {
    const e = document.createElement("div");
    e.classList.add("x", "y", "x");
    const one = e.className;
    e.classList.add();
    e.className = " z  z ";
    e.classList.add("w");
    return [one, e.className].join("|");
  });
  run("classList add empty set", () => { const e = document.createElement("div"); e.classList.add(); return [e.hasAttribute("class"), JSON.stringify(e.getAttribute("class"))].join("|"); });
  run("classList remove", () => {
    const e = document.createElement("div");
    e.classList.remove("a");
    const missing = e.hasAttribute("class");
    e.className = " a b  a c ";
    e.classList.remove("a", "q");
    return [missing, JSON.stringify(e.className)].join("|");
  });
  run("classList toggle", () => {
    const e = document.createElement("div");
    const r1 = e.classList.toggle("a");
    const r2 = e.classList.toggle("a");
    const attr = JSON.stringify(e.getAttribute("class"));
    const r3 = e.classList.toggle("b", false);
    const r4 = e.classList.toggle("b", true);
    const r5 = e.classList.toggle("b", true);
    return [r1, r2, attr, r3, r4, r5, e.className].join("|");
  });
  run("classList toggle force absent", () => { const e = document.createElement("div"); const r = e.classList.toggle("a", false); return [r, e.hasAttribute("class")].join("|"); });
  run("classList replace", () => {
    const e = document.createElement("div");
    e.className = "a b c b";
    const r1 = e.classList.replace("b", "d");
    const s1 = e.className;
    const r2 = e.classList.replace("q", "z");
    const s2 = e.className;
    e.className = "a b c";
    const r3 = e.classList.replace("c", "a");
    return [r1, s1, r2, s2, r3, e.className].join("|");
  });
  for (const [m, args] of [["add", [""]], ["add", ["a b"]], ["remove", [""]], ["remove", ["a\tb"]], ["toggle", [""]], ["toggle", ["a b"]], ["replace", ["", "a"]], ["replace", ["a", ""]], ["replace", ["a b", "c"]], ["replace", ["a", "c d"]], ["contains", [""]]]) {
    run(`classList ${m} ${JSON.stringify(args)}`, () => { const e = document.createElement("div"); e.className = "a"; return e.classList[m](...args); });
  }
  run("classList supports", () => document.createElement("div").classList.supports("a"));
  run("classList value setter", () => { const e = document.createElement("div"); e.classList.value = " q  r "; return [e.className, e.classList.length].join("|"); });
  run("classList put forwards", () => { const e = document.createElement("div"); e.classList = "a  b"; return [e.className, e.classList.length].join("|"); });
  run("classList put forwards undefined", () => { const e = document.createElement("div"); e.classList = undefined; return e.className; });
  run("classList put forwards no arg", () => { const d = Object.getOwnPropertyDescriptor(Element.prototype, "classList"); const e = document.createElement("div"); d.set.call(e); return e.className; });
  run("classList put forwards shadowed", () => {
    const e = document.createElement("div");
    Object.defineProperty(e, "classList", { get() { return { set value(v) { this.got = v; } }; }, configurable: true });
    const d = Object.getOwnPropertyDescriptor(Element.prototype, "classList");
    d.set.call(e, "z");
    return e.className;
  });
  run("classList put forwards not object", () => {
    const e = document.createElement("div");
    Object.defineProperty(e, "classList", { get() { return 5; } });
    const d = Object.getOwnPropertyDescriptor(Element.prototype, "classList");
    d.set.call(e, "z");
  });
  run("classList no args", () => document.createElement("div").classList.toggle());
  run("classList item no args", () => document.createElement("div").classList.item());
  run("classList assign index", () => { "use strict"; document.createElement("div").classList[0] = "a"; });
  run("part", () => { const e = document.createElement("div"); e.part.add("x"); e.part = "y z"; return [e.getAttribute("part"), e.part.length, e.part === e.part].join("|"); });
  run("classList on svg", () => { const e = document.createElementNS("http://www.w3.org/2000/svg", "rect"); e.classList.add("q"); return e.getAttribute("class"); });
  run("classList quirks", () => { const e = document.createElement("div"); e.className = "A a"; return e.classList.length; });
  // Every name's interface.
  for (const name of HTML_NAMES) {
    run(`html ${name}`, () => document.createElement(name));
    run(`html ns ${name}`, () => document.createElementNS(HTML, name));
  }
  run("html interfaces", () => {
    const names = ["a", "img", "input", "canvas", "video", "audio", "table", "td", "th", "col", "thead", "dialog", "details", "template", "q", "blockquote", "ins", "del", "button", "select", "option", "form"];
    return names.map((n) => { const e = document.createElement(n); return n + ":" + Object.getPrototypeOf(e).constructor.name + ":" + e.tagName; }).join(",");
  });
  run("new HTMLAnchorElement", () => new HTMLAnchorElement());
  run("Image", () => {
    const i = new Image();
    const j = new Image(3);
    const k = new Image(3, 4);
    return [i, i.nodeName, i.width, i.height, i.outerHTML, j.outerHTML, k.width, k.height, k.outerHTML, Object.getPrototypeOf(i) === HTMLImageElement.prototype, i instanceof Image, Image.prototype === HTMLImageElement.prototype, Image.name, Image.length, i.ownerDocument === document].map(String).join("|");
  });
  run("Image args", () => [new Image(-1).outerHTML, new Image(2 ** 31).outerHTML, new Image(2 ** 32 + 5).outerHTML, new Image("7", null).outerHTML, new Image(undefined, 2).outerHTML, new Image(1.9).width].join("|"));
  run("Image as function", () => Image());
  run("Image bad arg", () => new Image(Symbol()));
  run("Audio", () => {
    const a = new Audio();
    const b = new Audio("x.mp3");
    const c = new Audio(undefined);
    const d = new Audio(null);
    return [a, a.outerHTML, b.outerHTML, c.outerHTML, d.outerHTML, Audio.length, Audio.name, a instanceof HTMLMediaElement, Audio.prototype === HTMLAudioElement.prototype].map(String).join("|");
  });
  run("Audio as function", () => Audio());
  run("Option", () => typeof Option + "|" + Option.length);
  run("img width", () => {
    const i = document.createElement("img");
    const out = [i.width, i.height];
    i.setAttribute("width", " 12px");
    i.setAttribute("height", "-0");
    out.push(i.width, i.height);
    i.setAttribute("width", "4294967295");
    i.setAttribute("height", "4294967296");
    out.push(i.width, i.height);
    i.setAttribute("width", "-3");
    out.push(i.width);
    i.width = 3000000000;
    out.push(i.getAttribute("width"));
    i.height = -1;
    out.push(i.getAttribute("height"));
    return out.join(",");
  });
  run("img reflected", () => { const i = document.createElement("img"); i.src = "  #a "; i.alt = "x"; i.hspace = 5; i.crossOrigin = "USE-credentials"; return [i.getAttribute("src"), i.alt, i.hspace, i.crossOrigin, i.loading, i.decoding, i.fetchPriority, i.referrerPolicy, i.useMap, i.isMap].join("|"); });
  run("anchor href", () => {
    const a = blank.createElementNS(HTML, "a");
    const out = [a.href, String(a)];
    a.setAttribute("href", " http://Example.COM/a b?x#y ");
    out.push(a.href, String(a), a.toString(), JSON.stringify(a.href));
    a.href = "#frag";
    out.push(a.getAttribute("href"), a.href);
    a.href = "http://[bad";
    out.push(a.href);
    return out.join("|");
  });
  run("anchor reflected", () => { const a = document.createElement("a"); a.target = "_blank"; a.download = "f"; a.rel = "noopener"; a.hreflang = "en"; a.type = "t"; a.referrerPolicy = "NO-REFERRER"; a.ping = " http://p "; return [a.target, a.download, a.rel, a.hreflang, a.type, a.referrerPolicy, a.ping, a.name, a.coords, a.charset, a.rev, a.shape, a.getAttributeNames().join(" ")].join("|"); });
  run("area href", () => { const a = blank.createElementNS(HTML, "area"); a.href = "http://e.com/x"; return [a.href, String(a), a.alt, a.coords, a.shape, a.noHref].join("|"); });
  // A link's href that does not resolve is the attribute as it is, where a reflected URL loses its spaces.
  run("href invalid spaces", () => {
    const a = document.createElement("a");
    a.setAttribute("href", " http://[bad ");
    const area = document.createElement("area");
    area.setAttribute("href", "\t http://[bad \n");
    const link = document.createElement("link");
    link.setAttribute("href", " http://[bad ");
    const img = document.createElement("img");
    img.setAttribute("src", " http://[bad ");
    return JSON.stringify([a.href, String(a), area.href, link.href, img.src]);
  });
  run("table elements", () => { const t = document.createElement("td"); t.colSpan; return [document.createElement("th").tagName, t.abbr, t.headers, t.align, t.noWrap, document.createElement("col").span === undefined].join("|"); });
  run("canvas", () => { const c = document.createElement("canvas"); const out = [c, c.width, c.height]; c.setAttribute("width", "-5"); out.push(c.width); c.setAttribute("width", " 20px"); out.push(c.width); c.width = 3000000000; out.push(c.getAttribute("width")); c.height = 7; out.push(c.height); return out.join("|"); });
  run("canvas large attribute", () => { const c = document.createElement("canvas"); c.setAttribute("width", "3000000000"); c.setAttribute("height", "2147483647"); const out = [c.width, c.height]; c.setAttribute("height", "2147483648"); out.push(c.height); return out.join("|"); });
  run("media preload", () => { const v = document.createElement("video"); const out = [v.preload]; for (const x of ["none", "AUTO", "", "metadata", "bogus"]) { v.setAttribute("preload", x); out.push(v.preload); } v.preload = "Q"; out.push(v.getAttribute("preload"), document.createElement("audio").preload, new Audio().preload); return out.join(","); });
  run("Image undefined", () => [new Image(undefined).outerHTML, new Image(undefined, undefined).outerHTML].join("|"));
  run("video", () => { const v = document.createElement("video"); return [v, v.poster, v.playsInline, v.autoplay, v.controls, v.loop, v.muted, v.defaultMuted, v.preload, v.crossOrigin, v.src].map(String).join("|"); });
  run("form controls exist", () => ["button", "input", "select", "textarea", "option", "optgroup", "fieldset", "legend", "label", "output", "datalist", "form", "meter", "progress"].map((n) => Object.prototype.toString.call(document.createElement(n))).join(","));
  run("button reflected", () => { const b = document.createElement("button"); b.disabled = true; b.name = "n"; b.formNoValidate = true; return [b.disabled, b.name, b.autofocus, b.formTarget, b.getAttributeNames().join(" ")].join("|"); });
  run("dialog details", () => { const d = document.createElement("dialog"); const s = document.createElement("details"); d.open = true; s.open = true; s.name = "g"; return [d.open, d.outerHTML, s.open, s.name, s.outerHTML].join("|"); });
  run("parsed interfaces", () => { const d = document.createElement("div"); d.innerHTML = "<a href=x>a</a><img><canvas></canvas><table><tbody><tr><td>c</td></tr></tbody></table><video></video><dialog></dialog>"; const out = []; for (let n = d.firstChild; n; n = n.nextSibling) out.push(Object.prototype.toString.call(n)); return out.join(","); });
  run("ol start", () => { const o = document.createElement("ol"); return [o.reversed, o.type, o.compact].join("|"); });
  // The parts of a link's URL.
  const parts = (e) => ["href", "origin", "protocol", "username", "password", "host", "hostname", "port", "pathname", "search", "hash"].map((p) => p + "=" + e[p]).join(" ");
  const hrefs = [null, "", "https://user:pa%20ss@Example.COM:8080/a/../b/c?q=1&r#frag", "http://example.com:80/", "https://example.com:443/x", "ftp://f.example:21/", "ws://w.example:81/", "file:///C:/dir/f.txt", "data:text/plain,hi", "javascript:void(0)", "mailto:a@b.example", "http://[::1]:8000/p", "http://[bad", "http://example.com/%7Euser?%20x#%20y", "blob:https://example.com/uuid", "about:blank#f", "http://a.example/\u00e9?\u00e9#\u00e9", "  http://sp.example/ ", "x/y", "?q", "#h", "http://example.com:99999/", "http://ex ample.com/", "sc://opaque/path?q#f", "sc:opaque", "http://xn--nxasmq6b.example/", "http://\u00e9.example/"];
  for (const value of hrefs) {
    run(`a parts ${JSON.stringify(value)}`, () => { const a = blank.createElementNS(HTML, "a"); if (value !== null) a.setAttribute("href", value); return parts(a); });
  }
  run("area parts", () => { const a = blank.createElementNS(HTML, "area"); a.href = "https://e.example:1/p?q#h"; return parts(a); });
  const setters = [
    ["protocol", ["https", "HTTPS:junk", "ftp", "file", "foo", "1x", "", "ws:"]],
    ["username", ["u", "u s", "", "@:/"]],
    ["password", ["p", "", "p:w"]],
    ["host", ["h.example", "h.example:81", "h.example:80", ":81", "", "h.example:0081x", "[::2]:9", "a/b", "h\t.example"]],
    ["hostname", ["n.example", "n.example:5", "", "a b", "N.EXAMPLE"]],
    ["port", ["81", "443", "80", "", "0081x", "x", "65536", "08080", "000"]],
    ["pathname", ["/p q", "x", "", "/a/../b", "\\\\c"]],
    ["search", ["q=1", "?", "", "??x", "a b", "#"]],
    ["hash", ["h", "#h", "", "#", "a b", "\u00e9"]],
  ];
  for (const [name, values] of setters) {
    for (const value of values) {
      run(`a set ${name} ${JSON.stringify(value)}`, () => {
        const a = blank.createElementNS(HTML, "a");
        a.href = "http://user:pw@host.example:8080/path?query#frag";
        a[name] = value;
        return a.getAttribute("href");
      });
    }
  }
  run("a set on no href", () => { const a = blank.createElementNS(HTML, "a"); a.protocol = "https"; a.hash = "x"; a.search = "y"; a.host = "h"; a.pathname = "p"; return String(a.getAttribute("href")); });
  run("a set on opaque", () => { const a = blank.createElementNS(HTML, "a"); a.href = "mailto:x@y.example"; a.host = "h"; a.pathname = "p"; a.port = "1"; a.search = "s"; a.hash = "h"; return a.getAttribute("href"); });
  run("a set on invalid", () => { const a = blank.createElementNS(HTML, "a"); a.href = "http://[bad"; a.search = "s"; a.hash = "h"; a.protocol = "https"; a.pathname = "/p"; return a.getAttribute("href"); });
  run("a set on file", () => { const a = blank.createElementNS(HTML, "a"); a.href = "file:///x"; a.protocol = "http"; const one = a.getAttribute("href"); a.href = "http://u@h.example/"; a.protocol = "file"; return [one, a.getAttribute("href")].join("|"); });
  run("a set special to not", () => { const a = blank.createElementNS(HTML, "a"); a.href = "http://h.example/"; a.protocol = "foo"; const one = a.getAttribute("href"); a.href = "foo://h.example/"; a.protocol = "http"; return [one, a.getAttribute("href")].join("|"); });
  run("a toString", () => { const a = blank.createElementNS(HTML, "a"); a.href = "http://h.example/x"; return [String(a), a + "", `${a}`].join("|"); });
  // insertAdjacentHTML, importNode, adoptNode.
  run("insertAdjacentHTML", () => {
    const p = document.createElement("div");
    const e = document.createElement("span");
    p.appendChild(e);
    e.insertAdjacentHTML("beforebegin", "<i>1</i>");
    e.insertAdjacentHTML("AFTERBEGIN", "<b>2</b>x");
    e.insertAdjacentHTML("beforeEnd", "<u>3</u>");
    e.insertAdjacentHTML("afterend", "<em>4</em>");
    return p.innerHTML;
  });
  run("insertAdjacentHTML no parent", () => document.createElement("span").insertAdjacentHTML("afterend", "x"));
  run("insertAdjacentHTML fragment parent", () => { const f = document.createDocumentFragment(); const e = document.createElement("i"); f.appendChild(e); e.insertAdjacentHTML("beforebegin", "x"); });
  run("insertAdjacentHTML bad", () => document.createElement("span").insertAdjacentHTML("middle", "x"));
  run("insertAdjacentHTML no args", () => document.createElement("span").insertAdjacentHTML("afterbegin"));
  run("importNode", () => {
    const doc = new Document();
    const e = doc.createElementNS(HTML, "div");
    e.appendChild(doc.createTextNode("t"));
    const shallow = document.importNode(e);
    const deep = document.importNode(e, true);
    const opts = document.importNode(e, {});
    const self = document.importNode(e, { selfOnly: true });
    return [shallow.ownerDocument === document, shallow.childNodes.length, deep.childNodes.length, opts.childNodes.length, self.childNodes.length, e.ownerDocument === doc, deep === e].join("|");
  });
  run("importNode document", () => document.importNode(new Document()));
  run("importNode template", () => { const t = document.createElement("template"); t.innerHTML = "<p>x</p>"; const c = document.importNode(t.content, true); return [c.childNodes.length, c.ownerDocument === document, c.firstChild.textContent].join("|"); });
  run("importNode no args", () => document.importNode());
  run("importNode null", () => document.importNode(null));
  run("adoptNode", () => {
    const doc = new Document();
    const p = doc.createElementNS(HTML, "div");
    const c = doc.createElementNS(HTML, "span");
    p.appendChild(c);
    const r = document.adoptNode(c);
    return [r === c, c.ownerDocument === document, p.childNodes.length, c.parentNode].map(String).join("|");
  });
  run("adoptNode document", () => document.adoptNode(new Document()));
  run("adoptNode attr", () => { const e = document.createElement("div"); e.setAttribute("a", "1"); const a = e.getAttributeNode("a"); const doc = new Document(); doc.adoptNode(a); return [a.ownerElement, a.ownerDocument === doc, e.hasAttribute("a"), a.value].map(String).join("|"); });
  // HTMLElement members.
  run("tabIndex defaults", () => ["div", "a", "area", "button", "input", "select", "textarea", "iframe", "frame", "object", "span", "summary", "details", "img"].map((n) => n + ":" + document.createElement(n).tabIndex).join(",") + "|" + document.createElementNS(SVG, "a").tabIndex + "," + document.createElementNS(SVG, "rect").tabIndex + "," + document.createElementNS(MATHML, "mi").tabIndex);
  run("tabIndex summary", () => { const d = document.createElement("details"); const s1 = document.createElement("summary"); const s2 = document.createElement("summary"); d.append(s1, s2); return [s1.tabIndex, s2.tabIndex].join(","); });
  run("tabIndex parse", () => { const e = document.createElement("div"); const out = []; for (const v of ["3", " -2x", "abc", "", "99999999999"]) { e.setAttribute("tabindex", v); out.push(e.tabIndex); } e.tabIndex = 7; out.push(e.getAttribute("tabindex")); const a = document.createElement("a"); a.setAttribute("tabindex", "bad"); out.push(a.tabIndex); return out.join(","); });
  run("dir", () => { const e = document.createElement("div"); const out = [JSON.stringify(e.dir)]; for (const v of ["LTR", "rtl", "Auto", "x", ""]) { e.setAttribute("dir", v); out.push(e.dir); } e.dir = "Q"; out.push(e.getAttribute("dir")); return out.join(","); });
  run("translate", () => { const p = document.createElement("div"); const c = document.createElement("span"); p.appendChild(c); const out = [c.translate]; p.setAttribute("translate", "NO"); out.push(c.translate); c.setAttribute("translate", ""); out.push(c.translate); c.setAttribute("translate", "maybe"); out.push(c.translate); c.translate = true; out.push(c.getAttribute("translate")); const s = document.createElementNS(SVG, "g"); s.setAttribute("translate", "no"); const h = document.createElement("b"); s.appendChild(h); out.push(h.translate); return out.join(","); });
  run("draggable", () => { const out = [document.createElement("div").draggable, document.createElement("img").draggable, document.createElement("a").draggable]; const a = document.createElement("a"); a.href = "x"; out.push(a.draggable); a.setAttribute("draggable", "FALSE"); out.push(a.draggable); const i = document.createElement("img"); i.setAttribute("draggable", "false"); out.push(i.draggable); const d = document.createElement("div"); d.setAttribute("draggable", "True"); out.push(d.draggable); d.draggable = false; out.push(d.getAttribute("draggable")); return out.join(","); });
  run("spellcheck", () => { const p = document.createElement("div"); const c = document.createElement("span"); p.appendChild(c); const out = [c.spellcheck]; p.setAttribute("spellcheck", "false"); out.push(c.spellcheck); c.setAttribute("spellcheck", ""); out.push(c.spellcheck); c.setAttribute("spellcheck", "x"); out.push(c.spellcheck); c.spellcheck = true; out.push(c.getAttribute("spellcheck")); return out.join(","); });
  // A password input's spellcheck is off by default.
  run("spellcheck password", () => {
    const d = document.createElement("div");
    d.setAttribute("spellcheck", "true");
    const i = d.appendChild(document.createElement("input"));
    i.setAttribute("type", "password");
    const out = [i.spellcheck];
    const j = document.createElement("input");
    j.setAttribute("type", "PASSWORD");
    out.push(j.spellcheck);
    j.setAttribute("spellcheck", "true");
    out.push(j.spellcheck);
    out.push(document.createElement("input").spellcheck);
    const inner = document.createElement("span");
    const p = document.createElement("input");
    p.setAttribute("type", "password");
    p.appendChild(inner);
    out.push(inner.spellcheck);
    return out.join(",");
  });
  // Blink's HasBeenPasswordField(): off while it stays a text field after being a password one, and a copy has only
  // the type it has.
  run("spellcheck password sticky", () => {
    const i = document.createElement("input");
    i.setAttribute("type", "password");
    i.type = "text";
    const out = [i.spellcheck];
    i.setAttribute("type", "search");
    out.push(i.spellcheck);
    i.removeAttribute("type");
    out.push(i.spellcheck, i.cloneNode().spellcheck);
    i.setAttribute("type", "checkbox");
    out.push(i.spellcheck);
    i.setAttribute("type", "text");
    out.push(i.spellcheck);
    const p = document.createElement("input");
    p.type = "Password";
    out.push(p.cloneNode().spellcheck);
    const d = document.createElement("div");
    d.innerHTML = "<input type=password>";
    const q = d.firstChild;
    q.type = "email";
    out.push(q.spellcheck);
    q.setAttribute("type", "date");
    out.push(q.spellcheck);
    q.type = "number";
    out.push(q.spellcheck);
    return out.join(",");
  });
  run("contentEditable", () => { const e = document.createElement("div"); const out = [e.contentEditable]; for (const v of ["", "TRUE", "false", "plaintext-only", "x"]) { e.setAttribute("contenteditable", v); out.push(e.contentEditable); } e.contentEditable = "PlainText-Only"; out.push(e.getAttribute("contenteditable")); e.contentEditable = "inherit"; out.push(e.hasAttribute("contenteditable")); return out.join(","); });
  run("contentEditable bad", () => { document.createElement("div").contentEditable = "yes"; });
  run("isContentEditable", () => {
    const p = document.createElement("div");
    const c = document.createElement("span");
    p.appendChild(c);
    p.contentEditable = "true";
    const detached = c.isContentEditable;
    document.body.appendChild(p);
    const out = [detached, p.isContentEditable, c.isContentEditable];
    c.contentEditable = "false";
    out.push(c.isContentEditable);
    c.contentEditable = "plaintext-only";
    out.push(c.isContentEditable);
    p.remove();
    return out.join(",");
  });
  run("isContentEditable design mode", () => {
    const p = document.createElement("div");
    p.innerHTML = "<span contenteditable=false><b></b></span>";
    const detached = p.firstChild;
    document.body.appendChild(p);
    const out = [];
    try {
      document.designMode = "on";
      out.push(detached.isContentEditable, detached.firstChild.isContentEditable, document.documentElement.isContentEditable);
      p.remove();
      out.push(detached.isContentEditable);
    } finally {
      document.designMode = "off";
    }
    return out.join(",");
  });
  run("isContentEditable foreign", () => {
    const p = document.createElement("div");
    p.contentEditable = "true";
    const svg = p.appendChild(document.createElementNS(SVG, "svg"));
    const inSvg = svg.appendChild(document.createElement("span"));
    const fo = svg.appendChild(document.createElementNS(SVG, "foreignObject"));
    const inFo = fo.appendChild(document.createElement("span"));
    const g = svg.appendChild(document.createElementNS(SVG, "g"));
    const inG = g.appendChild(document.createElement("span"));
    const math = p.appendChild(document.createElementNS(MATHML, "math"));
    const inMath = math.appendChild(document.createElement("span"));
    const mi = math.appendChild(document.createElementNS(MATHML, "mi"));
    const inMi = mi.appendChild(document.createElement("span"));
    const other = p.appendChild(document.createElementNS("urn:x", "svg"));
    const inOther = other.appendChild(document.createElement("span"));
    const own = fo.appendChild(document.createElement("i"));
    own.contentEditable = "true";
    const out = [inSvg, inFo, inG, inMath, inMi, inOther, own].map((e) => e.isContentEditable);
    document.body.appendChild(p);
    out.push(inFo.isContentEditable, inSvg.isContentEditable);
    p.remove();
    return out.join(",");
  });
  run("isContentEditable fragment", () => {
    const f = document.createDocumentFragment();
    const e = f.appendChild(document.createElement("span"));
    const t = document.createElement("template");
    t.innerHTML = "<div contenteditable><b></b></div>";
    return [e.isContentEditable, t.content.firstChild.firstChild.isContentEditable].join(",");
  });
  // dataset.
  run("dataset read", () => {
    const e = document.createElement("div");
    e.setAttribute("data-foo", "1");
    e.setAttribute("data-foo-bar", "2");
    e.setAttribute("data--x", "3");
    e.setAttribute("data-a1-b", "4");
    e.setAttribute("data-", "5");
    e.setAttribute("datax", "6");
    e.setAttributeNS("urn:x", "data-ns", "7");
    const d = e.dataset;
    return [d, d === e.dataset, d.foo, d.fooBar, d["-x"], d.a1B, d["a1-b"], d[""], d.datax, d.ns, d.nope, JSON.stringify(Object.keys(d)), JSON.stringify(Object.getOwnPropertyNames(d)), "foo" in d, d.hasOwnProperty("fooBar"), JSON.stringify(Object.getOwnPropertyDescriptor(d, "foo"))].map(String).join("|");
  });
  run("dataset write", () => {
    const e = document.createElement("div");
    const d = e.dataset;
    d.fooBar = 1;
    d.x = null;
    d.Up = "u";
    d.toString = "t";
    d["a-1"] = "n";
    return [e.getAttributeNames().join(" "), e.getAttribute("data-foo-bar"), e.getAttribute("data-x"), JSON.stringify(Object.keys(d)), typeof d.toString, Object.prototype.toString.call(d)].join("|");
  });
  run("dataset bad name", () => { document.createElement("div").dataset["a-b"] = 1; });
  run("dataset bad attribute", () => { document.createElement("div").dataset["a b"] = 1; });
  run("dataset delete", () => {
    const e = document.createElement("div");
    e.setAttribute("data-foo", "1");
    const d = e.dataset;
    const r1 = delete d.foo;
    const r2 = delete d.missing;
    const r3 = delete d["a-b"];
    return [r1, r2, r3, e.hasAttribute("data-foo"), JSON.stringify(Object.keys(d))].join("|");
  });
  run("dataset define", () => {
    const d = document.createElement("div").dataset;
    Object.defineProperty(d, "x", { value: "v" });
    let accessor = "ok";
    try { Object.defineProperty(d, "y", { get() { return 1; } }); } catch (e) { accessor = e.name + ": " + e.message; }
    return [d.x, accessor, JSON.stringify(Object.keys(d))].join("|");
  });
  run("dataset override builtins", () => {
    const e = document.createElement("div");
    e.setAttribute("data-has-own-property", "h");
    const d = e.dataset;
    return [typeof d.hasOwnProperty, d.hasOwnProperty].join("|");
  });
  run("dataset symbol", () => { const d = document.createElement("div").dataset; const s = Symbol("k"); d[s] = 1; return [d[s], Object.getOwnPropertySymbols(d).length].join("|"); });
  run("dataset svg", () => { const e = document.createElementNS(SVG, "rect"); e.dataset.k = "v"; return [e.getAttribute("data-k"), document.createElementNS(MATHML, "mi").dataset + ""].join("|"); });
  run("dataset strict delete", () => { "use strict"; const d = document.createElement("div").dataset; return delete d.nothing; });
  run("dataset json", () => { const e = document.createElement("div"); e.dataset.a = "1"; e.dataset.bC = "2"; return JSON.stringify(e.dataset) + "|" + JSON.stringify(Object.assign({}, e.dataset)); });
  // V8 calls the named setter only when the receiver is the map itself; a setter's error propagates through Reflect.
  run("dataset receiver", () => {
    const e = document.createElement("div");
    const child = Object.create(e.dataset);
    child.foo = "v";
    const r = {};
    const ok = Reflect.set(e.dataset, "k", "w", r);
    e.setAttribute("data-bar", "1");
    child.bar = "2";
    return [e.getAttribute("data-foo"), child.hasOwnProperty("foo"), child.foo, ok, e.hasAttribute("data-k"), r.k, e.getAttribute("data-bar"), child.hasOwnProperty("bar"), JSON.stringify(Object.keys(e.dataset))].join("|");
  });
  run("dataset reflect define", () => {
    const d = document.createElement("div").dataset;
    const a = Reflect.defineProperty(d, "x", { get() { return 1; } });
    const b = Reflect.defineProperty(d, "y", { value: "v" });
    const c = Reflect.defineProperty(d, "z", { value: "v", writable: false });
    const del = Reflect.deleteProperty(d, "y");
    return [a, b, c, d.x, d.y, d.z, del, JSON.stringify(Object.keys(d))].join("|");
  });
  run("dataset reflect set", () => {
    const d = document.createElement("div").dataset;
    return [Reflect.set(d, "a", "1"), Reflect.set(d, "a-b", "1")].join("|");
  });
  // Tables.
  const tableOf = (markup) => { const d = document.createElement("div"); d.innerHTML = "<table>" + markup + "</table>"; return d.firstChild; };
  const ids = (list) => Array.from(list, (e) => e.id || e.localName).join(",");
  run("table rows order", () => {
    const t = document.createElement("table");
    const add = (parent, name, id) => { const e = parent.appendChild(document.createElement(name)); if (id) e.id = id; return e; };
    const tb1 = add(t, "tbody");
    add(tb1, "tr", "b1");
    const tf = add(t, "tfoot");
    add(tf, "tr", "f1");
    add(t, "tr", "t1");
    const th = add(t, "thead");
    add(th, "tr", "h1");
    add(th, "td", "notrow");
    const th2 = add(t, "thead");
    add(th2, "tr", "h2");
    add(t, "div").appendChild(document.createElement("tr")).id = "nested";
    add(add(t, "tbody"), "tr", "b2");
    add(tb1, "tr", "b1b");
    t.appendChild(document.createElementNS(SVG, "tr")).id = "svg";
    const rows = t.rows;
    return [ids(rows), rows.length, rows === t.rows, ids(t.tBodies), t.tBodies === t.tBodies, t.rows.namedItem("t1").id, t.tHead === th, t.tFoot === tf].join("|");
  });
  run("table caption", () => {
    const t = tableOf("<tbody></tbody>");
    const out = [t.caption];
    const c = t.createCaption();
    out.push(c.localName, t.firstChild === c, t.createCaption() === c);
    const c2 = document.createElement("caption");
    t.caption = c2;
    out.push(t.firstChild === c2, c.parentNode, t.caption === c2);
    t.caption = null;
    out.push(t.caption, t.children.length);
    t.deleteCaption();
    return out.map(String).join("|");
  });
  run("table thead", () => {
    const t = document.createElement("table");
    t.appendChild(document.createElement("caption"));
    t.appendChild(document.createElement("colgroup"));
    t.appendChild(document.createTextNode("x"));
    t.appendChild(document.createElement("tbody"));
    const h = t.createTHead();
    const out = [Array.from(t.childNodes, (n) => n.nodeName).join(" "), t.createTHead() === h];
    const h2 = document.createElement("thead");
    t.tHead = h2;
    out.push(Array.from(t.childNodes, (n) => n.nodeName).join(" "), h.parentNode);
    t.deleteTHead();
    out.push(t.tHead);
    return out.map(String).join("|");
  });
  run("table thead wrong", () => { document.createElement("table").tHead = document.createElement("tbody"); });
  run("table tfoot wrong", () => { document.createElement("table").tFoot = document.createElement("thead"); });
  run("table caption wrong type", () => { document.createElement("table").caption = document.createElement("div"); });
  run("table tfoot", () => {
    const t = tableOf("<tbody><tr></tr></tbody>");
    const f = t.createTFoot();
    t.appendChild(document.createElement("tbody"));
    const out = [t.lastChild === f, t.createTFoot() === f];
    const f2 = document.createElement("tfoot");
    t.tFoot = f2;
    out.push(t.lastChild === f2, f.parentNode);
    t.deleteTFoot();
    out.push(t.tFoot, t.children.length);
    return out.map(String).join("|");
  });
  run("table createTBody", () => {
    const t = document.createElement("table");
    const b1 = t.createTBody();
    t.appendChild(document.createElement("tfoot"));
    const b2 = t.createTBody();
    return [Array.from(t.children, (e) => e.localName).join(" "), t.children[1] === b2, b1 !== b2].join("|");
  });
  run("table insertRow", () => {
    const t = document.createElement("table");
    const r0 = t.insertRow();
    const out = [t.innerHTML];
    t.insertRow(0).id = "a";
    t.insertRow(-1).id = "z";
    t.insertRow(2).id = "m";
    out.push(t.innerHTML);
    const h = t.createTHead();
    h.insertRow().id = "h";
    t.insertRow(1).id = "x";
    out.push(t.innerHTML, r0.rowIndex, r0.sectionRowIndex);
    return out.join("|");
  });
  run("table insertRow direct", () => { const t = tableOf(""); t.appendChild(document.createElement("tr")).id = "d"; t.insertRow().id = "n"; t.insertRow(0).id = "f"; return t.innerHTML; });
  run("table insertRow too big", () => document.createElement("table").insertRow(1));
  run("table insertRow too small", () => document.createElement("table").insertRow(-2));
  run("table deleteRow", () => {
    const t = tableOf("<thead><tr id=h></tr></thead><tbody><tr id=a></tr><tr id=b></tr></tbody><tfoot><tr id=f></tr></tfoot>");
    t.deleteRow(-1);
    const out = [ids(t.rows)];
    t.deleteRow(0);
    out.push(ids(t.rows));
    t.deleteRow(1);
    out.push(ids(t.rows));
    const e = document.createElement("table");
    e.deleteRow(-1);
    out.push(e.rows.length);
    return out.join("|");
  });
  run("table deleteRow too big", () => document.createElement("table").deleteRow(0));
  run("table deleteRow too small", () => document.createElement("table").deleteRow(-2));
  run("section rows", () => {
    const s = document.createElement("tbody");
    const out = [s.rows.length, s.rows === s.rows];
    s.insertRow().id = "a";
    s.insertRow(0).id = "b";
    s.insertRow(1).id = "c";
    s.appendChild(document.createElement("td"));
    out.push(ids(s.rows));
    s.deleteRow(-1);
    s.deleteRow(0);
    out.push(ids(s.rows));
    return out.join("|");
  });
  run("section insertRow bad", () => document.createElement("thead").insertRow(2));
  run("section deleteRow bad", () => document.createElement("tfoot").deleteRow(0));
  run("section deleteRow empty", () => { const s = document.createElement("tfoot"); s.deleteRow(-1); return s.rows.length; });
  run("row indices", () => {
    const t = tableOf("<thead><tr id=h></tr></thead><tbody><tr id=a></tr><tr id=b></tr></tbody><tr id=d></tr>");
    const out = Array.from(t.rows, (r) => r.rowIndex + "/" + r.sectionRowIndex);
    const lone = document.createElement("tr");
    out.push(lone.rowIndex, lone.sectionRowIndex);
    const div = document.createElement("div").appendChild(document.createElement("tbody"));
    const inner = div.appendChild(document.createElement("tr"));
    out.push(inner.rowIndex, inner.sectionRowIndex);
    return out.join(",");
  });
  run("row cells", () => {
    const r = document.createElement("tr");
    const out = [r.cells.length, r.cells === r.cells];
    r.insertCell().id = "a";
    r.insertCell(0).id = "b";
    r.appendChild(document.createElement("th")).id = "h";
    r.appendChild(document.createElement("div"));
    r.insertCell(2).id = "c";
    out.push(ids(r.cells), Array.from(r.cells, (c) => c.cellIndex).join(""), r.insertCell().localName);
    r.deleteCell(-1);
    r.deleteCell(0);
    out.push(ids(r.cells));
    out.push(document.createElement("td").cellIndex);
    return out.join("|");
  });
  run("row insertCell bad", () => document.createElement("tr").insertCell(1));
  run("row deleteCell bad", () => document.createElement("tr").deleteCell(0));
  run("row deleteCell low", () => document.createElement("tr").deleteCell(-2));
  run("row deleteCell empty", () => { const r = document.createElement("tr"); r.appendChild(document.createElement("td")); r.deleteCell(-1); r.deleteCell(-1); });
  run("cell spans", () => {
    const c = document.createElement("td");
    const out = [c.colSpan, c.rowSpan];
    for (const v of ["0", "3", "2000", "99999999999", "-1", "-0", " 7x", "", "abc"]) {
      c.setAttribute("colspan", v);
      c.setAttribute("rowspan", v);
      out.push(c.colSpan + "/" + c.rowSpan);
    }
    c.setAttribute("rowspan", "70000");
    out.push(c.rowSpan);
    c.colSpan = 5;
    c.rowSpan = 0;
    out.push(c.getAttribute("colspan"), c.getAttribute("rowspan"));
    c.colSpan = 3000000000;
    c.rowSpan = -1;
    out.push(c.getAttribute("colspan"), c.getAttribute("rowspan"));
    return out.join(",");
  });
  run("col span", () => {
    const c = document.createElement("colgroup");
    const out = [c.span];
    for (const v of ["0", "4", "1001", "99999999999", "x"]) { c.setAttribute("span", v); out.push(c.span); }
    c.span = 0;
    out.push(c.getAttribute("span"));
    c.span = 4294967295;
    out.push(c.getAttribute("span"));
    return out.join(",");
  });
  run("cell cellIndex in table", () => { const t = tableOf("<tr><th></th><td id=x></td></tr>"); return t.querySelector("#x").cellIndex; });
  run("table reflected", () => { const t = document.createElement("table"); t.bgColor = null; t.cellPadding = 3; return [t.getAttribute("bgcolor"), t.cellPadding, t.border, document.createElement("td").scope].join("|"); });
  // Token lists of element classes.
  run("relList link", () => {
    const l = document.createElement("link");
    const r = l.relList;
    r.add("stylesheet", "icon");
    const out = [r === l.relList, l.getAttribute("rel"), r.length];
    for (const t of ["preload", "PRELOAD", "modulepreload", "noopener", "monetization", "compression-dictionary", "allowed-alt-sxg", "prerender", "expect", ""]) out.push(r.supports(t));
    l.relList = "a b";
    out.push(l.rel, String(l.relList));
    return out.join(",");
  });
  run("relList anchors", () => {
    const out = [];
    for (const [ns, name] of [[HTML, "a"], [HTML, "area"], [HTML, "form"], [SVG, "a"]]) {
      const e = document.createElementNS(ns, name);
      const r = e.relList;
      out.push(name + ":" + [r.supports("noopener"), r.supports("NoReferrer"), r.supports("opener"), r.supports("stylesheet"), r === e.relList].join("/"));
      r.toggle("noopener");
      out.push(e.getAttribute("rel"));
    }
    return out.join(",");
  });
  run("relList math a", () => "relList" in document.createElementNS(MATHML, "a"));
  run("link sizes", () => { const l = document.createElement("link"); l.sizes.add("16x16", "32x32"); return [l.getAttribute("sizes"), l.sizes === l.sizes, l.sizes.value].join("|"); });
  run("link sizes supports", () => document.createElement("link").sizes.supports("16x16"));
  run("blocking", () => {
    const out = [];
    for (const name of ["link", "style"]) {
      const e = document.createElement(name);
      out.push([e.blocking.supports("render"), e.blocking.supports("RENDER"), e.blocking.supports("full-frame-rate"), e.blocking === e.blocking].join("/"));
      e.blocking.add("render");
      out.push(e.getAttribute("blocking"));
    }
    return out.join(",");
  });
  run("iframe sandbox", () => {
    const f = document.createElement("iframe");
    const s = f.sandbox;
    const out = [s === f.sandbox, s.length];
    for (const t of ["allow-scripts", "ALLOW-SAME-ORIGIN", "allow-same-site-none-cookies", "allow-top-navigation-to-custom-protocols", "allow-storage-access-by-user-activation", "allow-presentation", "allow-everything"]) out.push(s.supports(t));
    f.sandbox = " allow-forms  allow-forms allow-popups ";
    out.push(s.length, s.value, f.getAttribute("sandbox"));
    s.remove("allow-forms");
    out.push(f.getAttribute("sandbox"));
    return out.join(",");
  });
  // hidden, getHTML() and document.implementation.
  run("hidden read", () => {
    const e = document.createElement("div");
    const out = [e.hidden];
    for (const v of ["", "UNTIL-FOUND", "until-found ", "x"]) {
      e.setAttribute("hidden", v);
      out.push(e.hidden);
    }
    return out.map((v) => JSON.stringify(v)).join(",");
  });
  run("hidden write", () => {
    const e = document.createElement("div");
    const out = [];
    for (const v of [true, false, "until-found", "Until-Found", "", "x", 0, 1, -1, NaN, -0, Infinity, null, undefined, {}, [], [1], "0", "false"]) {
      e.hidden = v;
      out.push(JSON.stringify(e.getAttribute("hidden")));
    }
    return out.join(",");
  });
  run("hidden types", () => { const e = document.createElement("div"); e.hidden = "until-found"; const a = typeof e.hidden; e.hidden = 1; return [a, typeof e.hidden, e.hidden, e.outerHTML].join("|"); });
  run("hidden foreign", () => ["hidden" in document.createElementNS(SVG, "g"), "hidden" in document.createElementNS(MATHML, "mi")].join("|"));
  run("hidden symbol", () => { document.createElement("div").hidden = Symbol("s"); });
  run("getHTML", () => {
    const d = document.createElement("div");
    d.innerHTML = "<p a=1>x&amp;<template><i>t</i></template></p><!--c-->";
    return [d.getHTML(), d.getHTML({}), d.getHTML({ serializableShadowRoots: true, shadowRoots: [] }), d.getHTML() === d.innerHTML, d.getHTML(null), d.getHTML(undefined)].join("|");
  });
  run("getHTML template", () => { const t = document.createElement("template"); t.innerHTML = "<b>x</b>"; return t.getHTML(); });
  run("getHTML shadowRoots", () => document.createElement("div").getHTML({ shadowRoots: [document.body] }));
  run("getHTML not object", () => document.createElement("div").getHTML(5));
  run("getHTML not iterable", () => document.createElement("div").getHTML({ shadowRoots: 5 }));
  run("implementation", () => {
    const i = document.implementation;
    const d = new Document();
    return [i === document.implementation, String(i), i.hasFeature(), i.hasFeature("Core", "2.0"), d.implementation === i, d.implementation === d.implementation].join("|");
  });
  run("createDocumentType", () => { const t = document.implementation.createDocumentType("html", "-//W3C//DTD", "about:x"); return [t.name, t.publicId, t.systemId, t.ownerDocument === document, t.parentNode, t.nodeName, String(t)].join("|"); });
  run("createDocumentType names", () => ["a:b", "1a", "", "aé", "a<", "a "].map((n) => { try { return JSON.stringify(document.implementation.createDocumentType(n, "", "").name); } catch (e) { return e.name; } }).join(","));
  run("createDocumentType space", () => document.implementation.createDocumentType("a b", "", ""));
  run("createDocumentType gt", () => document.implementation.createDocumentType("a>", "", ""));
  run("createDocumentType null char", () => document.implementation.createDocumentType("a\u0000", "", ""));
  run("createDocumentType args", () => document.implementation.createDocumentType("a"));
  run("createHTMLDocument", () => {
    const d = document.implementation.createHTMLDocument("T<");
    return [d.documentElement.outerHTML, d.contentType, d.compatMode, d.URL, d.characterSet, String(d), d.doctype.name, d.title, d.defaultView, d.implementation === document.implementation, d.childNodes.length, d.body.localName].join("|");
  });
  run("createHTMLDocument no title", () => {
    const i = document.implementation;
    return [i.createHTMLDocument().documentElement.outerHTML, i.createHTMLDocument(undefined).head.childNodes.length, i.createHTMLDocument(null).title, i.createHTMLDocument("").head.childNodes.length].join("|");
  });
  run("createHTMLDocument use", () => {
    const d = document.implementation.createHTMLDocument("x");
    const p = d.createElement("P");
    d.body.appendChild(p);
    p.innerHTML = "<b>y</b>";
    const imported = document.importNode(d.body, true);
    return [p.localName, p.namespaceURI, d.getElementsByTagName("b").length, d.querySelector("b").textContent, imported.outerHTML, d.cloneNode(true).body.innerHTML, String(d.cloneNode())].join("|");
  });
  run("createDocument", () => {
    const d = document.implementation.createDocument(HTML, "html", null);
    return [String(d), d.contentType, d.documentElement.localName, d.documentElement.namespaceURI, String(d.documentElement), d.childNodes.length, d.URL, d.characterSet, d.compatMode, d.doctype, d.defaultView, d.createElement("P").namespaceURI, d.createElement("P").localName, String(d.createElement("p")), String(d.cloneNode()), d.cloneNode().contentType].join("|");
  });
  run("createDocument svg", () => {
    const d = document.implementation.createDocument(SVG, "svg:svg");
    return [d.contentType, d.documentElement.prefix, d.documentElement.localName, String(d.documentElement), d.createElement("rect").namespaceURI, String(d.cloneNode()), d.cloneNode().contentType].join("|");
  });
  run("createDocument other", () => {
    const i = document.implementation;
    const d = i.createDocument("urn:x", "a:b");
    const e = i.createDocument(null, "");
    const n = i.createDocument(null, null, undefined);
    return [d.contentType, d.documentElement.prefix, d.documentElement.namespaceURI, e.contentType, e.documentElement, e.childNodes.length, n.childNodes.length, e.createElement("P").namespaceURI, e.createElement("P").localName, String(e.createElement("p"))].join("|");
  });
  run("createDocument doctype", () => {
    const t = document.implementation.createDocumentType("a", "p", "s");
    const d = document.implementation.createDocument("urn:x", "a", t);
    return [d.contentType, d.firstChild === t, t.ownerDocument === d, d.lastChild.localName, d.doctype === t, d.childNodes.length].join("|");
  });
  run("createDocument moves doctype", () => {
    const d1 = document.implementation.createHTMLDocument();
    const t = d1.doctype;
    const d = document.implementation.createDocument(null, "x", t);
    return [d1.doctype, t.ownerDocument === d, d.doctype === t].join("|");
  });
  run("createDocument bad prefix", () => document.implementation.createDocument(null, "a:b"));
  run("createDocument bad name", () => document.implementation.createDocument("urn:x", "1a"));
  run("createDocument xmlns", () => document.implementation.createDocument("urn:x", "xmlns:a"));
  run("createDocument args", () => document.implementation.createDocument());
  run("createDocument doctype type", () => document.implementation.createDocument(null, "a", document.body));
  run("createDocument types", () => {
    const i = document.implementation;
    return [i.createDocument(null, "") instanceof XMLDocument, i.createDocument(null, "") instanceof Document, i.createHTMLDocument() instanceof HTMLDocument, Object.getPrototypeOf(i.createDocument(null, "")) === XMLDocument.prototype].join("|");
  });
  run("XMLDocument constructor", () => new XMLDocument());
  return JSON.stringify(cases, null, 1);
})()
