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
  // SVG's animated strings, booleans, enumerations, integers and numbers.
  const SVG_ANIMATED = [["a","target","String","target"],["clipPath","clipPathUnits","Enumeration","clipPathUnits"],["feFuncR","type","Enumeration","type"],["feFuncR","slope","Number","slope"],["feFuncR","intercept","Number","intercept"],["feFuncR","amplitude","Number","amplitude"],["feFuncR","exponent","Number","exponent"],["feFuncR","offset","Number","offset"],["rect","className","String","class"],["feBlend","in1","String","in"],["feBlend","in2","String","in2"],["feBlend","mode","Enumeration","mode"],["feColorMatrix","in1","String","in"],["feColorMatrix","type","Enumeration","type"],["feComponentTransfer","in1","String","in"],["feComposite","in2","String","in2"],["feComposite","in1","String","in"],["feComposite","operator","Enumeration","operator"],["feComposite","k1","Number","k1"],["feComposite","k2","Number","k2"],["feComposite","k3","Number","k3"],["feComposite","k4","Number","k4"],["feConvolveMatrix","in1","String","in"],["feConvolveMatrix","orderX","Integer","order"],["feConvolveMatrix","orderY","Integer","order"],["feConvolveMatrix","divisor","Number","divisor"],["feConvolveMatrix","bias","Number","bias"],["feConvolveMatrix","targetX","Integer","targetX"],["feConvolveMatrix","targetY","Integer","targetY"],["feConvolveMatrix","edgeMode","Enumeration","edgeMode"],["feConvolveMatrix","kernelUnitLengthX","Number","kernelUnitLength"],["feConvolveMatrix","kernelUnitLengthY","Number","kernelUnitLength"],["feConvolveMatrix","preserveAlpha","Boolean","preserveAlpha"],["feDiffuseLighting","in1","String","in"],["feDiffuseLighting","surfaceScale","Number","surfaceScale"],["feDiffuseLighting","diffuseConstant","Number","diffuseConstant"],["feDiffuseLighting","kernelUnitLengthX","Number","kernelUnitLength"],["feDiffuseLighting","kernelUnitLengthY","Number","kernelUnitLength"],["feDisplacementMap","in1","String","in"],["feDisplacementMap","in2","String","in2"],["feDisplacementMap","scale","Number","scale"],["feDisplacementMap","xChannelSelector","Enumeration","xChannelSelector"],["feDisplacementMap","yChannelSelector","Enumeration","yChannelSelector"],["feDistantLight","azimuth","Number","azimuth"],["feDistantLight","elevation","Number","elevation"],["feDropShadow","in1","String","in"],["feDropShadow","dx","Number","dx"],["feDropShadow","dy","Number","dy"],["feDropShadow","stdDeviationX","Number","stdDeviation"],["feDropShadow","stdDeviationY","Number","stdDeviation"],["feGaussianBlur","in1","String","in"],["feGaussianBlur","stdDeviationX","Number","stdDeviation"],["feGaussianBlur","stdDeviationY","Number","stdDeviation"],["feMergeNode","in1","String","in"],["feMorphology","in1","String","in"],["feMorphology","operator","Enumeration","operator"],["feMorphology","radiusX","Number","radius"],["feMorphology","radiusY","Number","radius"],["feOffset","in1","String","in"],["feOffset","dx","Number","dx"],["feOffset","dy","Number","dy"],["fePointLight","x","Number","x"],["fePointLight","y","Number","y"],["fePointLight","z","Number","z"],["feSpecularLighting","in1","String","in"],["feSpecularLighting","surfaceScale","Number","surfaceScale"],["feSpecularLighting","specularConstant","Number","specularConstant"],["feSpecularLighting","specularExponent","Number","specularExponent"],["feSpecularLighting","kernelUnitLengthX","Number","kernelUnitLength"],["feSpecularLighting","kernelUnitLengthY","Number","kernelUnitLength"],["feSpotLight","x","Number","x"],["feSpotLight","y","Number","y"],["feSpotLight","z","Number","z"],["feSpotLight","pointsAtX","Number","pointsAtX"],["feSpotLight","pointsAtY","Number","pointsAtY"],["feSpotLight","pointsAtZ","Number","pointsAtZ"],["feSpotLight","specularExponent","Number","specularExponent"],["feSpotLight","limitingConeAngle","Number","limitingConeAngle"],["feTile","in1","String","in"],["feTurbulence","baseFrequencyX","Number","baseFrequency"],["feTurbulence","baseFrequencyY","Number","baseFrequency"],["feTurbulence","numOctaves","Integer","numOctaves"],["feTurbulence","seed","Number","seed"],["feTurbulence","stitchTiles","Enumeration","stitchTiles"],["feTurbulence","type","Enumeration","type"],["filter","filterUnits","Enumeration","filterUnits"],["filter","primitiveUnits","Enumeration","primitiveUnits"],["feFlood","result","String","result"],["path","pathLength","Number","pathLength"],["linearGradient","gradientUnits","Enumeration","gradientUnits"],["linearGradient","spreadMethod","Enumeration","spreadMethod"],["marker","markerUnits","Enumeration","markerUnits"],["mask","maskUnits","Enumeration","maskUnits"],["mask","maskContentUnits","Enumeration","maskContentUnits"],["pattern","patternUnits","Enumeration","patternUnits"],["pattern","patternContentUnits","Enumeration","patternContentUnits"],["stop","offset","Number","offset"],["text","lengthAdjust","Enumeration","lengthAdjust"],["textPath","method","Enumeration","method"],["textPath","spacing","Enumeration","spacing"],["a","href","String","href"],["textPath","side","Enumeration","side"]];
  const svg = (name) => document.createElementNS(SVG, name);
  const each = (f) => SVG_ANIMATED.map(([tag, member, kind, attribute]) => {
    try {
      return tag + "." + member + "=" + f(svg(tag), member, kind, attribute);
    } catch (e) {
      return tag + "." + member + " threw " + e.name + ": " + e.message;
    }
  }).join(" | ");
  run("svg animated defaults", () => each((e, m) => [Object.prototype.toString.call(e[m]), e[m] === e[m], JSON.stringify(e[m].baseVal), JSON.stringify(e[m].animVal)].join("/")));
  run("svg animated invalid", () => each((e, m, kind, a) => { if (kind === "String") return "-"; e.setAttribute(a, "bad"); return e[m].baseVal; }));
  run("svg animated empty", () => each((e, m, kind, a) => { e.setAttribute(a, ""); return JSON.stringify(e[m].baseVal); }));
  run("svg animated enumerations", () => each((e, m, kind, a) => {
    if (kind !== "Enumeration") return "-";
    const out = [];
    for (let v = 1; v < 20; v++) {
      try { e[m].baseVal = v; out.push(e.getAttribute(a) + ":" + e[m].baseVal + ":" + e[m].animVal); } catch (x) { out.push(x.message); break; }
    }
    return out.join(",");
  }));
  run("svg animated set", () => each((e, m, kind, a) => {
    const value = { String: "v w", Boolean: true, Enumeration: 1, Integer: 7, Number: 2.25 }[kind];
    e[m].baseVal = value;
    return [e.getAttribute(a), e[m].baseVal, e[m].animVal, e.attributes.length].join(",");
  }));
  run("svg animated prototypes", () => ["String", "Boolean", "Enumeration", "Integer", "Number"].map((k) => {
    const I = window["SVGAnimated" + k];
    let made;
    try { new I(); made = "made"; } catch (x) { made = x.name + ": " + x.message; }
    return [k, I.length, Object.getOwnPropertyNames(I.prototype).join(" "), made].join("/");
  }).join(" | "));
  run("svg number parse", () => {
    const p = svg("path");
    return ["5", " 5", "5 ", "\t5\n", "5,", "5 ,", ",5", "1e2", "1E2", "1e+2", "1e-2", "1e", "1ex", "1em", "abc", "-1.5", "+.5e-1", "-.5", "0x10", "1.", ".", ".e1", "5px", "Infinity", "NaN", "1e38", "1e39", "3e38", "1e-50", "3.14159265358979", "0.1", "123456789012", "00012", "1 2", "+-1", "--1", "1.5.5", "\u00a05"].map((v) => { p.setAttribute("pathLength", v); return JSON.stringify(v) + "=" + p.pathLength.baseVal; }).join(",");
  });
  run("svg number set", () => {
    const p = svg("path");
    return [0.1, 1 / 3, 2 / 3, 1e20, -0, 123456789, 1e-7, 2.5, 100, 1e6, 999999.5, 1234567, 0.000123, 0.00001234, -42.125, 3e38, "7", true, null].map((v) => { p.pathLength.baseVal = v; return String(v) + "=" + p.getAttribute("pathLength") + "/" + p.pathLength.baseVal; }).join(",");
  });
  run("svg number errors", () => {
    const p = svg("path");
    const out = [];
    for (const v of [NaN, Infinity, -Infinity, "x", undefined, {}]) {
      try { p.pathLength.baseVal = v; out.push("ok " + p.getAttribute("pathLength")); } catch (x) { out.push(x.name + ": " + x.message); }
    }
    return out.join(",");
  });
  run("svg number percentage", () => {
    const s = svg("stop");
    return ["50%", "0.5", "150%", "-1", "2", "bad", "50 %", " 50%", "50% ", "50%%", "%", "1e2%", "50,"].map((v) => { s.setAttribute("offset", v); return JSON.stringify(v) + "=" + s.offset.baseVal; }).concat([(s.offset.baseVal = 0.25, s.getAttribute("offset"))]).join(",");
  });
  run("svg integer parse", () => {
    const t = svg("feTurbulence");
    return ["3", "3.7", "-2", "1e1", "abc", " 4", "4 ", "+5", "2147483647", "2147483648", "-2147483648", "-2147483649", "07", "-0", "+-1", "1 2", "\u00a06", "6\u3000", "\v8"].map((v) => { t.setAttribute("numOctaves", v); return JSON.stringify(v) + "=" + t.numOctaves.baseVal; }).join(",");
  });
  run("svg integer set", () => {
    const t = svg("feTurbulence");
    return [3.7, -2.5, 2147483648, 4294967297, "9", NaN, Infinity].map((v) => { t.numOctaves.baseVal = v; return String(v) + "=" + t.numOctaves.baseVal + "/" + t.getAttribute("numOctaves"); }).join(",");
  });
  run("svg number pairs", () => {
    const g = svg("feGaussianBlur");
    const out = ["3", "3 4", "3,4", "3 , 4", "3 4 5", "bad", "3 bad", " 2  5 ", "3,", ",3", "", "1e1 2e1"].map((v) => { g.setAttribute("stdDeviation", v); return JSON.stringify(v) + "=" + g.stdDeviationX.baseVal + "," + g.stdDeviationY.baseVal; });
    g.setAttribute("stdDeviation", "1 2");
    const x = g.stdDeviationX;
    x.baseVal = 7;
    out.push(g.getAttribute("stdDeviation"), g.stdDeviationY.baseVal);
    g.setAttribute("stdDeviation", "3");
    g.stdDeviationY.baseVal = 8;
    out.push(g.getAttribute("stdDeviation"));
    g.stdDeviationX.baseVal = 8;
    out.push(g.getAttribute("stdDeviation"));
    g.setStdDeviation(5, 6);
    out.push(g.getAttribute("stdDeviation"), x === g.stdDeviationX, g.stdDeviationX !== g.stdDeviationY);
    return out.join(",");
  });
  run("svg order", () => {
    const c = svg("feConvolveMatrix");
    const out = ["3", "3 2", "2.5", "2.9 1.1", "0", "-1 2", "2 0", "bad", "1e1", "4,4", "2147483648"].map((v) => { c.setAttribute("order", v); return JSON.stringify(v) + "=" + c.orderX.baseVal + "," + c.orderY.baseVal; });
    c.removeAttribute("order");
    c.orderX.baseVal = 4;
    out.push(c.getAttribute("order"));
    c.orderY.baseVal = 4;
    out.push(c.getAttribute("order"));
    c.orderX.baseVal = 0;
    out.push(c.getAttribute("order"), c.orderX.baseVal, c.orderY.baseVal);
    return out.join(",");
  });
  run("svg boolean", () => {
    const c = svg("feConvolveMatrix");
    const b = c.preserveAlpha;
    const out = ["true", "false", "TRUE", "", "yes", " true", "1"].map((v) => { c.setAttribute("preserveAlpha", v); return JSON.stringify(v) + "=" + b.baseVal; });
    for (const v of [true, 0, "x", "", null]) { b.baseVal = v; out.push(c.getAttribute("preserveAlpha") + ":" + b.baseVal); }
    return out.join(",");
  });
  run("svg enumeration values", () => {
    const b = svg("feBlend");
    const out = ["multiply", "MULTIPLY", "bad", "", "hue", " screen"].map((v) => { b.setAttribute("mode", v); return JSON.stringify(v) + "=" + b.mode.baseVal; });
    for (const v of [0, 1, 16, 17, -1, 65536, 65537, 2.5, "3", NaN]) {
      try { b.mode.baseVal = v; out.push(v + "=" + b.mode.baseVal + ":" + b.getAttribute("mode")); } catch (x) { out.push(v + " " + x.name + ": " + x.message); }
    }
    const c = svg("feComposite");
    c.setAttribute("operator", "lighter");
    out.push(c.operator.baseVal, c.operator.animVal);
    return out.join(",");
  });
  run("svg class name", () => {
    const r = svg("rect");
    const c = r.className;
    const out = [String(c), c.baseVal === ""];
    r.setAttribute("class", "a b");
    out.push(c.baseVal, c.animVal);
    c.baseVal = "x y";
    out.push(r.getAttribute("class"), r.classList.length, r.classList.contains("y"), r.matches(".x"));
    c.baseVal = null;
    out.push(r.getAttribute("class"));
    r.removeAttribute("class");
    out.push(JSON.stringify(c.baseVal));
    const h = document.createElement("div");
    out.push(typeof h.className, Object.getOwnPropertyDescriptor(SVGElement.prototype, "className").set === undefined);
    out.push((function () { "use strict"; try { c.animVal = "q"; return "no error"; } catch (x) { return x.name; } })());
    return out.join(",");
  });
  run("svg href", () => {
    const out = [];
    const a = svg("a");
    out.push(JSON.stringify(a.href.baseVal));
    a.setAttributeNS(XLINK, "xlink:href", "x");
    out.push(a.href.baseVal);
    a.setAttribute("href", "y");
    out.push(a.href.baseVal, a.href.animVal);
    a.removeAttribute("href");
    out.push(a.href.baseVal);
    a.href.baseVal = "z";
    out.push(a.getAttribute("href"), a.getAttributeNS(XLINK, "href"), a.attributes.length);
    a.removeAttributeNS(XLINK, "href");
    out.push(JSON.stringify(a.href.baseVal));
    a.href.baseVal = "w";
    out.push(a.getAttribute("href"), a.attributes.length);
    for (const name of ["use", "image", "linearGradient", "radialGradient", "pattern", "filter", "textPath", "feImage", "script", "mpath"]) {
      const e = svg(name);
      e.setAttributeNS(XLINK, "xlink:href", "#x");
      out.push(name + ":" + e.href.baseVal);
    }
    out.push("href" in svg("rect"), "result" in svg("rect"), "result" in svg("feFlood"));
    return out.join(",");
  });
  run("svg animated strings", () => {
    const b = svg("feBlend");
    b.in1.baseVal = "SourceGraphic";
    b.in2.baseVal = 5;
    b.result.baseVal = "r";
    return [b.getAttribute("in"), b.getAttribute("in1"), b.in2.baseVal, b.getAttribute("in2"), b.getAttribute("result"), b.attributes.length].join(",");
  });
  run("svg animated after attribute", () => {
    const f = svg("feFuncR");
    const s = f.slope;
    const out = [s.baseVal];
    f.setAttribute("slope", "2");
    out.push(s.baseVal);
    s.baseVal = 1 / 3;
    out.push(f.getAttribute("slope"), s.baseVal);
    f.setAttribute("slope", "0.333333");
    out.push(s.baseVal);
    f.setAttributeNS(null, "slope", "4");
    out.push(s.baseVal);
    f.attributes.getNamedItem("slope").value = "5";
    out.push(s.baseVal);
    f.removeAttribute("slope");
    out.push(s.baseVal);
    f.setAttributeNS("urn:x", "x:slope", "6");
    out.push(s.baseVal);
    f.innerHTML = "";
    f.setAttribute("SLOPE", "7");
    out.push(s.baseVal, f.attributes.length);
    return out.join(",");
  });
  run("svg animated copies", () => {
    const f = svg("feFuncR");
    f.slope.baseVal = 1 / 3;
    const copy = f.cloneNode();
    return [copy.getAttribute("slope"), copy.slope.baseVal, copy.slope === f.slope, f.slope.baseVal].join(",");
  });
  run("svg animated mutations", () => {
    const r = svg("rect");
    const p = svg("path");
    const f = svg("feGaussianBlur");
    const o = new MutationObserver(() => {});
    for (const e of [r, p, f]) o.observe(e, { attributes: true, attributeOldValue: true });
    r.className.baseVal = "a";
    p.pathLength.baseVal = 3;
    f.stdDeviationX.baseVal = 2;
    const quiet = o.takeRecords().length;
    r.setAttribute("class", "b");
    p.getAttribute("pathLength");
    return [quiet, o.takeRecords().map((x) => x.attributeName + ":" + x.oldValue).join(" ")].join(",");
  });
  // SVG's lengths.
  const lengthOf = (l) => { let value; try { value = l.value; } catch (e) { value = e.name + ": " + e.message; } return [l.unitType, l.valueInSpecifiedUnits, JSON.stringify(l.valueAsString), value].join("/"); };
  const tryIt = (f) => { try { return String(f()); } catch (e) { return e.name + ": " + e.message; } };
  run("svg length parse", () => {
    const r = svg("rect");
    return ["5", "5px", " 5px ", "5PX", "5e1px", "+.5cm", "-1mm", "2in", "3pt", "4pc", "1q", "1Q", "10%", "2em", "3ex", "1rem", "1ch", "10vw", "1lh", "5 px", "px", "", "abc", "5px 6px", "inherit", "1e40", "0.1234567", "123456789", "1e-7px", "1e40px", "-0", "5deg", "5s", "#5", "calc(10px + 1px)", "calc(1in - 1px)", "calc(10px + 1em)", "min(1px, 2px)", "max(1cm, 1px)", "clamp(1px, 2px, 3px)", "calc(5)", "calc(1deg)", "calc(1px", "var(--x)", " var(--x, 5px) ", "calc(var(--x) * 2)", "var(--x) !important", "var(--x);", "{var(--x)}", "VAR(--y)"].map((v) => { r.setAttribute("x", v); return JSON.stringify(v) + "=" + lengthOf(r.x.baseVal); }).join(" | ");
  });
  run("svg length connected", () => {
    const s = svg("svg");
    s.setAttribute("width", "200");
    s.setAttribute("height", "100");
    (document.body || document.documentElement).appendChild(s);
    const r = svg("rect");
    const c = svg("circle");
    const inner = svg("svg");
    inner.setAttribute("width", "50%");
    inner.setAttribute("height", "40");
    const deep = svg("rect");
    inner.appendChild(deep);
    s.append(r, c, inner);
    const out = [];
    for (const v of ["10%", "2em", "1rem", "calc(10px + 1em)", "3in"]) { r.setAttribute("x", v); out.push("x " + v + "=" + r.x.baseVal.value); }
    r.setAttribute("y", "10%"); out.push("y=" + r.y.baseVal.value);
    r.setAttribute("width", "25%"); out.push("width=" + r.width.baseVal.value);
    c.setAttribute("r", "10%"); out.push("r=" + c.r.baseVal.value);
    deep.setAttribute("x", "10%"); deep.setAttribute("y", "50%"); out.push("deep=" + deep.x.baseVal.value + "," + deep.y.baseVal.value);
    out.push("svg=" + [s.width.baseVal.value, s.height.baseVal.value, s.x.baseVal.value].join(","));
    s.setAttribute("viewBox", "0 0 400 50"); r.setAttribute("x", "10%"); out.push("viewBox=" + r.x.baseVal.value + "," + r.y.baseVal.value);
    s.removeAttribute("viewBox");
    r.setAttribute("x", "10%"); r.x.baseVal.value = 50; out.push("set%=" + r.getAttribute("x"));
    r.setAttribute("x", "2em"); r.x.baseVal.value = 8; out.push("setEm=" + r.getAttribute("x"));
    r.setAttribute("x", "30px"); r.x.baseVal.convertToSpecifiedUnits(2); out.push("to%=" + r.getAttribute("x"));
    r.x.baseVal.convertToSpecifiedUnits(3); out.push("toEm=" + r.getAttribute("x"));
    r.x.baseVal.convertToSpecifiedUnits(5); out.push("toPx=" + r.getAttribute("x"));
    s.remove();
    out.push("removed=" + tryIt(() => r.x.baseVal.value));
    return out.join(" | ");
  });
  run("svg length nested", () => {
    const outer = svg("svg");
    outer.setAttribute("width", "200");
    outer.setAttribute("height", "100");
    (document.body || document.documentElement).appendChild(outer);
    let parent = outer;
    for (let i = 0; i < 40; i++) {
      const next = svg("svg");
      parent.appendChild(next);
      parent = next;
    }
    const c = svg("circle");
    c.setAttribute("cx", "10%");
    c.setAttribute("cy", "10%");
    c.setAttribute("r", "10%");
    parent.appendChild(c);
    const out = [c.cx.baseVal.value, c.cy.baseVal.value, c.r.baseVal.value];
    outer.remove();
    return out.join(",");
  });
  run("svg length references", () => {
    const r = svg("rect");
    const l = r.x.baseVal;
    return ["var()", "var(foo)", "var(--)", "var(--x)", "var(-x)", "var(--x foo)", "var(--x,)", "var( --x )", "var(--x , 1px)", "var(--x, var(y))", "var(--x, var(--y))", "var(--x, ;)", "var(--x, {})", "var(--x, ))", "var(--x,!)", "var(--x) var(--y)", "1px var(--x)", "env(foo)", "env(foo, 1px)", "env(1)", "env(foo 1)", "env(foo 1 2)", "env(foo -1)", "env(foo 1.5)", "env()", "attr(x)", "attr(x px)", "attr(x type(<length>))", "attr()", "attr(1)", "if(x)", "if(supports(x: y): 1px)", "--f()", "--f(1px)", "inherit(--x)", "var(--x) {}", "{var(--x)} {}", "{var(--x)}", " {var(--x)}", "{var(--x)} ", "[var(--x)]", "(var(--x))", "var(--x) )", "var(--x) ]", "VAR(--X)", "var(--x)!", "var(--x) ! important", "var(--x)!important", "calc(var(--x))", "url(var(--x))", "var(--x,\"str\")", "var(--x, \"open", "var(--x"].map((v) => {
      r.setAttribute("x", v);
      const fromAttribute = l.unitType + "/" + JSON.stringify(l.valueAsString);
      let set;
      try { l.valueAsString = "5"; l.valueAsString = v; set = JSON.stringify(l.valueAsString); } catch (e) { set = e.name; }
      return JSON.stringify(v) + "=" + fromAttribute + "/" + set;
    }).join(" | ");
  });
  run("svg length set", () => {
    const e = svg("rect");
    const l = e.x.baseVal;
    e.setAttribute("x", "5px");
    return [tryIt(() => { l.value = 7; return e.getAttribute("x") + " " + l.unitType; }), tryIt(() => { l.valueInSpecifiedUnits = 2.5; return e.getAttribute("x"); }), tryIt(() => { l.valueAsString = "3cm"; return e.getAttribute("x") + " " + l.value; }), tryIt(() => { l.valueAsString = "bad"; }), tryIt(() => { l.newValueSpecifiedUnits(7, 4); return e.getAttribute("x") + " " + l.value; }), tryIt(() => { l.newValueSpecifiedUnits(0, 4); }), tryIt(() => { l.newValueSpecifiedUnits(11, 4); }), tryIt(() => { l.convertToSpecifiedUnits(8); return e.getAttribute("x") + " " + l.valueInSpecifiedUnits; }), tryIt(() => { l.convertToSpecifiedUnits(3); }), tryIt(() => { l.convertToSpecifiedUnits(1); return e.getAttribute("x"); }), tryIt(() => { l.value = NaN; }), tryIt(() => { l.valueInSpecifiedUnits = Infinity; }), tryIt(() => { l.convertToSpecifiedUnits(0); }), tryIt(() => { l.newValueSpecifiedUnits(1, 1 / 3); return e.getAttribute("x") + " " + l.valueInSpecifiedUnits; }), tryIt(() => { l.newValueSpecifiedUnits(10, 1e20); return e.getAttribute("x") + " " + l.value; })].join(" | ");
  });
  run("svg length set relative and calc", () => {
    const e = svg("rect");
    const l = e.x.baseVal;
    e.setAttribute("x", "2em");
    const out = [tryIt(() => { l.value = 7; return e.getAttribute("x"); }), tryIt(() => { l.valueInSpecifiedUnits = 3; return e.getAttribute("x"); })];
    e.setAttribute("x", "calc(1px + 2px)");
    out.push(l.unitType, l.valueInSpecifiedUnits, tryIt(() => l.value), tryIt(() => { l.value = 7; return e.getAttribute("x") + " " + l.unitType; }));
    e.setAttribute("x", "calc(1px + 2px)");
    out.push(tryIt(() => { l.valueInSpecifiedUnits = 4; return e.getAttribute("x"); }));
    e.setAttribute("x", "var(--x)");
    out.push(tryIt(() => l.convertToSpecifiedUnits(5)), tryIt(() => { l.value = 2; return e.getAttribute("x"); }));
    return out.join(" | ");
  });
  run("svg length objects", () => {
    const e = svg("rect");
    const s = svg("svg");
    const c = s.createSVGLength();
    const out = [e.x.animVal === e.x.animVal, e.x.baseVal === e.x.baseVal, e.x.animVal !== e.x.baseVal, e.x === e.x, Object.prototype.toString.call(e.x), Object.prototype.toString.call(c), c !== s.createSVGLength()];
    for (const f of [() => { e.x.animVal.value = 1; }, () => { e.x.animVal.valueInSpecifiedUnits = 1; }, () => { e.x.animVal.valueAsString = "1"; }, () => { e.x.animVal.newValueSpecifiedUnits(1, 2); }, () => { e.x.animVal.convertToSpecifiedUnits(1); }]) out.push(tryIt(f));
    e.x.baseVal.value = 4;
    out.push(e.x.animVal.value, e.x.animVal.valueAsString);
    out.push(lengthOf(c), tryIt(() => { c.valueAsString = "2em"; return c.value; }), tryIt(() => { c.valueAsString = "10%"; return c.value; }), tryIt(() => { c.valueAsString = "3mm"; return c.value; }), tryIt(() => { c.convertToSpecifiedUnits(9); return c.valueAsString; }), tryIt(() => { c.value = 12; return c.valueAsString; }));
    out.push(SVGLength.SVG_LENGTHTYPE_UNKNOWN, SVGLength.SVG_LENGTHTYPE_PERCENTAGE, SVGLength.SVG_LENGTHTYPE_PC, c.SVG_LENGTHTYPE_EMS, tryIt(() => new SVGLength()), tryIt(() => new SVGAnimatedLength()));
    return out.join(",");
  });
  run("svg length defaults", () => [["rect", "x"], ["rect", "y"], ["rect", "width"], ["rect", "height"], ["rect", "rx"], ["rect", "ry"], ["circle", "cx"], ["circle", "cy"], ["circle", "r"], ["ellipse", "cx"], ["ellipse", "cy"], ["ellipse", "rx"], ["ellipse", "ry"], ["line", "x1"], ["line", "y1"], ["line", "x2"], ["line", "y2"], ["svg", "x"], ["svg", "y"], ["svg", "width"], ["svg", "height"], ["image", "x"], ["image", "width"], ["use", "y"], ["use", "height"], ["foreignObject", "x"], ["foreignObject", "height"], ["linearGradient", "x1"], ["linearGradient", "y1"], ["linearGradient", "x2"], ["linearGradient", "y2"], ["radialGradient", "cx"], ["radialGradient", "cy"], ["radialGradient", "r"], ["radialGradient", "fx"], ["radialGradient", "fy"], ["radialGradient", "fr"], ["mask", "x"], ["mask", "y"], ["mask", "width"], ["mask", "height"], ["pattern", "x"], ["pattern", "width"], ["filter", "x"], ["filter", "y"], ["filter", "width"], ["filter", "height"], ["marker", "refX"], ["marker", "refY"], ["marker", "markerWidth"], ["marker", "markerHeight"], ["text", "textLength"], ["tspan", "textLength"], ["textPath", "startOffset"], ["textPath", "textLength"], ...["feBlend", "feColorMatrix", "feComponentTransfer", "feComposite", "feConvolveMatrix", "feDiffuseLighting", "feDisplacementMap", "feDropShadow", "feFlood", "feGaussianBlur", "feImage", "feMerge", "feMorphology", "feOffset", "feSpecularLighting", "feTile", "feTurbulence"].flatMap((t) => ["x", "y", "width", "height"].map((m) => [t, m]))].map(([t, m]) => { const e = svg(t); const out = t + "." + m + "=" + lengthOf(e[m].baseVal); e.setAttribute(m, "bad"); return out + ";" + e[m].baseVal.valueAsString + "/" + e[m].baseVal.unitType; }).join(" | "));
  run("svg length text length", () => {
    const t = svg("text");
    const out = [lengthOf(t.textLength.baseVal)];
    t.setAttribute("textLength", "bad");
    out.push(lengthOf(t.textLength.baseVal));
    t.setAttribute("textLength", "5mm");
    out.push(lengthOf(t.textLength.baseVal));
    t.removeAttribute("textLength");
    t.textLength.baseVal.value = 9;
    out.push(t.getAttribute("textLength"), lengthOf(t.textLength.baseVal));
    return out.join(" | ");
  });
  run("svg length attributes", () => {
    const e = svg("rect");
    const o = new MutationObserver(() => {});
    o.observe(e, { attributes: true, attributeOldValue: true });
    const l = e.width.baseVal;
    l.value = 3;
    l.newValueSpecifiedUnits(6, 2);
    const quiet = o.takeRecords().length;
    e.setAttribute("width", "-5");
    const after = [l.valueAsString, l.value];
    e.setAttributeNS("urn:x", "x:width", "7");
    after.push(l.valueAsString);
    const copy = e.cloneNode();
    copy.width.baseVal.value = 10;
    after.push(e.getAttribute("width"), copy.getAttribute("width"));
    return [quiet, o.takeRecords().map((x) => x.attributeName + ":" + x.oldValue).join(" "), ...after].join(",");
  });
  // SVG's lists of numbers, lengths and strings.
  const listOf = (L, read) => [...Array(L.length).keys()].map((i) => read(L.getItem(i))).join(";");
  run("svg number list parse", () => {
    const m = svg("feColorMatrix");
    const L = m.values.baseVal;
    return ["1 2 3", "1,2,3", " 1 , 2 ,3 ", "1,,2", "1 2 bad", "", "1e2 .5", "1,", ",1", "1 2 ", "\t1\n2", "1.5e-3,-4"].map((v) => { m.setAttribute("values", v); return JSON.stringify(v) + "=" + L.length + ":" + listOf(L, (n) => n.value) + ":" + JSON.stringify(m.getAttribute("values")); }).join(" | ");
  });
  run("svg number list objects", () => {
    const m = svg("feColorMatrix");
    const s = svg("svg");
    const out = [Object.prototype.toString.call(m.values), Object.prototype.toString.call(m.values.baseVal), m.values.baseVal === m.values.baseVal, m.values.animVal === m.values.animVal, m.values.animVal !== m.values.baseVal, m.values === m.values];
    m.setAttribute("values", "1 2");
    const L = m.values.baseVal;
    const n = s.createSVGNumber();
    n.value = 1 / 3;
    const appended = L.appendItem(n);
    out.push(appended === n, L.length, m.getAttribute("values"), tryIt(() => { n.value = 9; return m.getAttribute("values"); }), tryIt(() => { appended.value = 8; return m.getAttribute("values") + " " + n.value; }));
    const first = L.getItem(0);
    out.push(first === L.getItem(0), first === L[0], tryIt(() => { first.value = 5; return m.getAttribute("values"); }));
    m.setAttribute("values", "7 8");
    out.push(first.value, tryIt(() => { first.value = 6; return m.getAttribute("values"); }), L.getItem(0).value);
    const other = svg("feColorMatrix");
    other.setAttribute("values", "1 2");
    const otherItem = other.values.baseVal.getItem(1);
    out.push(L.appendItem(otherItem) === otherItem, m.getAttribute("values"), other.getAttribute("values"), tryIt(() => { otherItem.value = 3; return m.getAttribute("values") + "|" + other.getAttribute("values"); }));
    out.push(L.appendItem(L.getItem(0)).value, m.getAttribute("values"), L.length);
    return out.join(",");
  });
  run("svg list items after the attribute", () => {
    const m = svg("feColorMatrix");
    m.setAttribute("values", "1 2");
    const L = m.values.baseVal;
    const n = L.getItem(0);
    m.setAttribute("values", "3 4");
    L.appendItem(n);
    const out = [m.getAttribute("values"), tryIt(() => { n.value = 9; return m.getAttribute("values"); })];
    const t = svg("text");
    t.setAttribute("x", "5%");
    const length = t.x.baseVal.getItem(0);
    const s = svg("svg");
    s.setAttribute("width", "200");
    (document.body || document.documentElement).appendChild(s);
    s.appendChild(t);
    out.push(tryIt(() => length.value));
    t.setAttribute("x", "1");
    out.push(tryIt(() => length.value), tryIt(() => { t.x.baseVal.insertItemBefore(length, 0); return t.getAttribute("x"); }), tryIt(() => { length.valueAsString = "7mm"; return t.getAttribute("x"); }));
    s.remove();
    return out.join(" | ");
  });
  run("svg number list operations", () => {
    const m = svg("feColorMatrix");
    const s = svg("svg");
    m.setAttribute("values", "1 2 3");
    const L = m.values.baseVal;
    const made = () => { const n = s.createSVGNumber(); n.value = 4; return n; };
    return [tryIt(() => L.insertItemBefore(made(), 99).value), m.getAttribute("values"), tryIt(() => L.insertItemBefore(made(), 0).value), m.getAttribute("values"), tryIt(() => L.replaceItem(made(), 1).value), m.getAttribute("values"), tryIt(() => L.removeItem(0).value), m.getAttribute("values"), tryIt(() => L.initialize(made()).value), m.getAttribute("values"), tryIt(() => L.removeItem(0).value), JSON.stringify(m.getAttribute("values")), tryIt(() => L.appendItem(made()).value), tryIt(() => { L.clear(); return L.length + " " + JSON.stringify(m.getAttribute("values")); }), tryIt(() => L.getItem(0)), tryIt(() => L[0]), tryIt(() => L.removeItem(0)), tryIt(() => L.replaceItem(made(), 0)), tryIt(() => L.appendItem(null)), tryIt(() => L.appendItem({ value: 1 })), tryIt(() => L.getItem(-1)), tryIt(() => L.insertItemBefore(made())), tryIt(() => L.getItem())].join(" | ");
  });
  run("svg number list indices", () => {
    const m = svg("feColorMatrix");
    const s = svg("svg");
    m.setAttribute("values", "1 2");
    const L = m.values.baseVal;
    const out = [tryIt(() => { L[0] = s.createSVGNumber(); return m.getAttribute("values"); }), tryIt(() => { L[5] = s.createSVGNumber(); return m.getAttribute("values") + " " + L.length; }), tryIt(() => { L[2] = s.createSVGNumber(); }), tryIt(() => { L[1] = 3; }), (function () { "use strict"; try { L[5] = s.createSVGNumber(); return "no error"; } catch (e) { return e.name + ": " + e.message; } })(), Object.keys(L).join(), 0 in L, 5 in L, tryIt(() => { delete L[0]; return L.length; }), JSON.stringify(Object.getOwnPropertyDescriptor(L, 0), (k, v) => (k === "value" ? typeof v : v)), tryIt(() => { Object.defineProperty(L, 0, { value: s.createSVGNumber() }); return m.getAttribute("values"); }), tryIt(() => { Object.defineProperty(L, 0, { get() { return 1; } }); })];
    const A = m.values.animVal;
    out.push(A.length, tryIt(() => A.clear()), tryIt(() => A.appendItem(s.createSVGNumber())), tryIt(() => { A.getItem(0).value = 3; }), tryIt(() => { A[0] = s.createSVGNumber(); return A.length; }), A.getItem(0) === A.getItem(0), A.getItem(0) !== L.getItem(0), A.getItem(0).value, tryIt(() => L.appendItem(A.getItem(0)).value), m.getAttribute("values"));
    const free = s.createSVGNumber();
    out.push(free.value, tryIt(() => { free.value = NaN; }), tryIt(() => { free.value = "2"; return free.value; }), Object.prototype.toString.call(free), tryIt(() => new SVGNumber()), tryIt(() => new SVGNumberList()));
    return out.join(" | ");
  });
  run("svg number list owners", () => ["feColorMatrix.values", "feFuncR.tableValues", "feFuncA.tableValues", "feConvolveMatrix.kernelMatrix", "text.rotate", "tspan.rotate", "textPath.rotate"].map((p) => { const [t, m] = p.split("."); const e = svg(t); return p + "=" + (e[m] === undefined ? "none" : Object.prototype.toString.call(e[m]) + "/" + e[m].baseVal.length); }).join(" | "));
  run("svg length list", () => {
    const t = svg("text");
    const L = t.x.baseVal;
    const out = [Object.prototype.toString.call(t.x), Object.prototype.toString.call(L), L === t.x.baseVal];
    for (const v of ["1 2px", "1,2px", " 1 2", "1,,2", "1 bad", "", "10% 2em 3in", "calc(1px + 1px) var(--x)", "calc(1px) 2", "1 2 ", "1,2,"]) { t.setAttribute("x", v); out.push(JSON.stringify(v) + "=" + L.length + ":" + listOf(L, (l) => l.valueAsString) + ":" + JSON.stringify(t.getAttribute("x"))); }
    t.setAttribute("x", "1 2px");
    const first = L.getItem(0);
    out.push(first.unitType, tryIt(() => { first.value = 3; return t.getAttribute("x"); }), tryIt(() => { first.newValueSpecifiedUnits(2, 50); return t.getAttribute("x"); }), tryIt(() => first.value));
    const s = svg("svg");
    const created = s.createSVGLength();
    created.value = 5;
    const appended = L.appendItem(created);
    out.push(appended === created, t.getAttribute("x"), tryIt(() => { created.value = 9; return t.getAttribute("x"); }));
    const r = svg("rect");
    r.setAttribute("x", "7mm");
    out.push(L.appendItem(r.x.baseVal).valueAsString, t.getAttribute("x"), tryIt(() => { r.x.baseVal.value = 1; return t.getAttribute("x") + "|" + r.getAttribute("x"); }));
    out.push(tryIt(() => L.removeItem(0).valueAsString), tryIt(() => { L.clear(); return JSON.stringify(t.getAttribute("x")); }));
    out.push(tryIt(() => { t.x.animVal.appendItem(created); }), tryIt(() => { t.setAttribute("x", "1"); t.x.animVal.getItem(0).value = 2; }));
    s.setAttribute("width", "200");
    (document.body || document.documentElement).appendChild(s);
    s.appendChild(t);
    t.setAttribute("x", "10%");
    t.setAttribute("y", "10% 20%");
    s.setAttribute("height", "50");
    out.push(tryIt(() => L.getItem(0).value), tryIt(() => t.y.baseVal.getItem(1).value), tryIt(() => t.dx.baseVal.length));
    s.remove();
    const span = svg("tspan");
    out.push([span.x, span.y, span.dx, span.dy].map((a) => a.baseVal.length).join(","));
    return out.join(" | ");
  });
  run("svg string list", () => {
    const g = svg("g");
    const R = g.requiredExtensions;
    const Y = g.systemLanguage;
    const out = [Object.prototype.toString.call(R), R.length, R === g.requiredExtensions, Y === g.systemLanguage, R !== Y];
    for (const v of ["a b", " a  b ", "a,b", "", "a\tb\nc", ",a,", " , "]) { g.setAttribute("systemLanguage", v); out.push("lang " + JSON.stringify(v) + "=" + Y.length + ":" + listOf(Y, (x) => x).replaceAll(";", "|")); }
    for (const v of ["x y", " x  y ", "x,y", "", "a\tb\nc"]) { g.setAttribute("requiredExtensions", v); out.push("ext " + JSON.stringify(v) + "=" + R.length + ":" + listOf(R, (x) => x).replaceAll(";", "|")); }
    g.setAttribute("requiredExtensions", "x y");
    out.push(R.appendItem("z"), g.getAttribute("requiredExtensions"), R.insertItemBefore("w", 0), g.getAttribute("requiredExtensions"), R.replaceItem("q", 1), R.removeItem(0), g.getAttribute("requiredExtensions"), R.initialize("only"), g.getAttribute("requiredExtensions"), tryIt(() => R.getItem(5)), tryIt(() => R[5]), R[0], tryIt(() => { R[0] = "set"; return g.getAttribute("requiredExtensions"); }), tryIt(() => { R.appendItem("a b"); return R.length + " " + g.getAttribute("requiredExtensions"); }), tryIt(() => R.removeItem(9)), tryIt(() => { R.clear(); return JSON.stringify(g.getAttribute("requiredExtensions")); }), tryIt(() => R.getItem(0)));
    g.setAttribute("systemLanguage", "en, fr");
    out.push(Y.appendItem("de"), g.getAttribute("systemLanguage"), tryIt(() => { Y[1] = 5; return g.getAttribute("systemLanguage"); }), tryIt(() => { Y.removeItem(0); Y.removeItem(0); Y.removeItem(0); return JSON.stringify(g.getAttribute("systemLanguage")); }));
    const o = new MutationObserver(() => {});
    o.observe(g, { attributes: true });
    R.appendItem("m");
    Y.appendItem("n");
    out.push(o.takeRecords().length);
    out.push(["a", "circle", "text", "use", "animate", "set", "svg", "switch", "foreignObject", "clipPath", "mask", "linearGradient", "pattern", "filter", "marker", "tspan", "image", "defs", "symbol", "stop", "view"].map((n) => n + ":" + (svg(n).requiredExtensions === undefined ? "-" : "+")).join(""));
    return out.join(" | ");
  });
  const rectOf = (r) => [r.x, r.y, r.width, r.height].join(",");
  run("svg view box", () => {
    const s = svg("svg");
    const v = s.viewBox;
    const R = v.baseVal;
    const out = [Object.prototype.toString.call(v), Object.prototype.toString.call(R), R === v.baseVal, v.animVal === v.animVal, v.animVal !== R, rectOf(R)];
    for (const value of ["1 2 3 4", "1,2,3,4", " 1 , 2 , 3 , 4 ", "1 2 3", "1 2 3 4 5", "1 2 -3 4", "1 2 3 -4", "a b c d", "", "1e2 .5 -1 2", "1 2 3 4,", "1,,2 3 4", "1\t2\n3\f4"]) { s.setAttribute("viewBox", value); out.push(JSON.stringify(value) + "=" + rectOf(R) + "/" + rectOf(v.animVal)); }
    s.setAttribute("viewBox", "1 2 3 4");
    R.x = 9;
    out.push(s.getAttribute("viewBox"), tryIt(() => { R.width = 1 / 3; return s.getAttribute("viewBox"); }), tryIt(() => { R.y = NaN; }), tryIt(() => { v.animVal.x = 1; }), rectOf(v.animVal));
    s.removeAttribute("viewBox");
    out.push(tryIt(() => { R.height = 5; return s.getAttribute("viewBox") + " " + rectOf(R); }));
    s.setAttribute("viewBox", "1 2 -3 4");
    out.push(tryIt(() => { R.x = 5; return s.getAttribute("viewBox"); }));
    const o = new MutationObserver(() => {});
    o.observe(s, { attributes: true });
    R.y = 7;
    out.push(o.takeRecords().length);
    const c = s.createSVGRect();
    out.push(Object.prototype.toString.call(c), rectOf(c), tryIt(() => { c.width = -2; return c.width; }), c !== s.createSVGRect(), tryIt(() => new SVGRect()));
    out.push(["svg", "symbol", "view", "pattern", "marker", "rect", "image", "g"].map((n) => n + ":" + (svg(n).viewBox ? "+" : "-")).join(""));
    return out.join(" | ");
  });
  run("svg view box sizes lengths", () => {
    const outer = (document.body || document.documentElement).appendChild(svg("svg"));
    const s = outer.appendChild(svg("svg"));
    const r = s.appendChild(svg("rect"));
    s.setAttribute("width", "300");
    s.setAttribute("height", "150");
    r.setAttribute("width", "50%");
    r.setAttribute("height", "50%");
    const out = [];
    for (const value of [null, "0 0 100 40", "0,0,100,40,", "0 0 -100 40", "0 0 0 40", "0 0 100 0", "5 5 20 10"]) {
      if (value === null) { s.removeAttribute("viewBox"); } else { s.setAttribute("viewBox", value); }
      out.push(JSON.stringify(value) + "=" + tryIt(() => r.width.baseVal.value + "," + r.height.baseVal.value));
    }
    s.setAttribute("viewBox", "0 0 100 40");
    s.viewBox.baseVal.width = 60;
    out.push(s.getAttribute("viewBox"), tryIt(() => r.width.baseVal.value));
    s.viewBox.baseVal.width = 1000001;
    out.push(s.getAttribute("viewBox"), tryIt(() => r.width.baseVal.value));
    s.removeAttribute("viewBox");
    s.height.baseVal.value = 1000003;
    out.push(s.getAttribute("height"), tryIt(() => r.height.baseVal.value));
    outer.remove();
    return out.join(" | ");
  });
  run("svg preserve aspect ratio", () => {
    const s = svg("svg");
    const p = s.preserveAspectRatio;
    const P = p.baseVal;
    const out = [Object.prototype.toString.call(p), Object.prototype.toString.call(P), P.align, P.meetOrSlice, P === p.baseVal, p.animVal === p.animVal, p.animVal !== P, SVGPreserveAspectRatio.SVG_PRESERVEASPECTRATIO_UNKNOWN, SVGPreserveAspectRatio.SVG_PRESERVEASPECTRATIO_XMAXYMAX, SVGPreserveAspectRatio.SVG_MEETORSLICE_SLICE];
    for (const value of ["none", "xMinYMin", "xMidYMid meet", "xMaxYMax slice", "defer xMinYMin", "XMINYMIN", "xMinYMin  slice", "xMinYMin,slice", "slice", "bad", "", " xMidYMax ", "none slice", "xMinYMin meet extra", "xMinYMinslice", "nonemeet", "xMaxYMid", "xMinYMax", "xMidYMin", "xMaxYMin", "xMinYMid", "xMidYMax", "xMinYMi", "xMaxYMax\tslice\n", "meet"]) { s.setAttribute("preserveAspectRatio", value); out.push(JSON.stringify(value) + "=" + P.align + "/" + P.meetOrSlice + "/" + p.animVal.align); }
    s.setAttribute("preserveAspectRatio", "xMinYMin");
    out.push(tryIt(() => { P.align = 10; return s.getAttribute("preserveAspectRatio"); }), tryIt(() => { P.meetOrSlice = 2; return s.getAttribute("preserveAspectRatio"); }), tryIt(() => { P.align = 0; }), tryIt(() => { P.align = 11; }), tryIt(() => { P.meetOrSlice = 0; }), tryIt(() => { P.meetOrSlice = 3; }), tryIt(() => { P.align = 1; return s.getAttribute("preserveAspectRatio"); }), tryIt(() => { p.animVal.align = 2; }), tryIt(() => { P.align = 65537; return s.getAttribute("preserveAspectRatio"); }));
    s.removeAttribute("preserveAspectRatio");
    out.push(tryIt(() => { P.meetOrSlice = 1; return s.getAttribute("preserveAspectRatio"); }));
    const o = new MutationObserver(() => {});
    o.observe(s, { attributes: true });
    P.align = 3;
    out.push(o.takeRecords().length);
    out.push(["svg", "symbol", "view", "pattern", "marker", "image", "feImage", "rect", "use"].map((n) => n + ":" + (svg(n).preserveAspectRatio ? svg(n).preserveAspectRatio.baseVal.align + "/" + svg(n).preserveAspectRatio.baseVal.meetOrSlice : "-")).join(" "));
    return out.join(" | ");
  });
  const angleOf = (a) => [a.unitType, a.value, a.valueInSpecifiedUnits, JSON.stringify(a.valueAsString)].join("/");
  run("svg angle", () => {
    const s = svg("svg");
    const m = svg("marker");
    const A = m.orientAngle.baseVal;
    const T = m.orientType;
    const out = [Object.prototype.toString.call(m.orientAngle), Object.prototype.toString.call(A), A === m.orientAngle.baseVal, m.orientAngle === m.orientAngle, m.orientAngle.animVal !== A, angleOf(A), SVGAngle.SVG_ANGLETYPE_UNKNOWN, SVGAngle.SVG_ANGLETYPE_GRAD];
    for (const value of ["45", "45deg", "1rad", "100grad", "0.5turn", "-30", "45 deg", "45DEG", "auto", "auto-start-reverse", "AUTO", "bad", "", "1e2", " 45deg ", "45deg ", ".5rad", "45\fdeg", "45 ", "4.5e1grad", "1/2"]) { m.setAttribute("orient", value); out.push(JSON.stringify(value) + "=" + T.baseVal + "/" + angleOf(A) + "/" + angleOf(m.orientAngle.animVal)); }
    m.setAttribute("orient", "45deg");
    out.push(tryIt(() => { A.value = 90; return m.getAttribute("orient") + " " + A.unitType; }), tryIt(() => { A.valueInSpecifiedUnits = 2; return m.getAttribute("orient"); }), tryIt(() => { A.newValueSpecifiedUnits(3, 1); return m.getAttribute("orient") + " " + A.value; }), tryIt(() => { A.convertToSpecifiedUnits(4); return m.getAttribute("orient") + " " + A.valueInSpecifiedUnits; }), tryIt(() => { A.convertToSpecifiedUnits(1); return m.getAttribute("orient"); }), tryIt(() => { A.convertToSpecifiedUnits(3); return m.getAttribute("orient"); }), tryIt(() => { A.convertToSpecifiedUnits(2); return m.getAttribute("orient"); }), tryIt(() => { A.valueAsString = "1turn"; }), m.getAttribute("orient"), angleOf(A), tryIt(() => { A.valueAsString = "bad"; }), tryIt(() => { A.valueAsString = ""; }), tryIt(() => { A.valueAsString = " 2grad"; return m.getAttribute("orient"); }), tryIt(() => { A.newValueSpecifiedUnits(0, 1); }), tryIt(() => { A.newValueSpecifiedUnits(5, 1); }), tryIt(() => { A.convertToSpecifiedUnits(0); }), tryIt(() => { A.convertToSpecifiedUnits(7); }), tryIt(() => { A.value = NaN; }), tryIt(() => { m.orientAngle.animVal.value = 1; }), tryIt(() => { m.orientAngle.animVal.newValueSpecifiedUnits(9, 1); }));
    m.setAttribute("orient", "1turn");
    out.push(angleOf(A), tryIt(() => { A.convertToSpecifiedUnits(4); return m.getAttribute("orient") + " " + angleOf(A); }));
    m.setAttribute("orient", "2turn");
    out.push(tryIt(() => { A.valueAsString = "3turn"; }), m.getAttribute("orient"), angleOf(A), tryIt(() => { A.value = 90; return m.getAttribute("orient"); }));
    m.setAttribute("orient", "10");
    A.valueInSpecifiedUnits = 1.23456789;
    out.push(m.getAttribute("orient"), tryIt(() => { A.valueAsString = "1turn"; }), A.valueInSpecifiedUnits, m.getAttribute("orient"));
    const a = s.createSVGAngle();
    out.push(Object.prototype.toString.call(a), angleOf(a), a !== s.createSVGAngle(), tryIt(() => new SVGAngle()));
    out.push(tryIt(() => { a.valueAsString = "auto"; return angleOf(a); }), tryIt(() => { a.valueAsString = "2rad"; return angleOf(a); }), tryIt(() => { a.convertToSpecifiedUnits(4); return angleOf(a); }), tryIt(() => { a.value = 180; return angleOf(a); }), tryIt(() => { a.valueAsString = "1turn"; }), angleOf(a), tryIt(() => { a.newValueSpecifiedUnits(2, 1 / 3); return angleOf(a); }));
    return out.join(" | ");
  });
  run("svg marker orient", () => {
    const s = svg("svg");
    const m = svg("marker");
    const A = m.orientAngle.baseVal;
    const T = m.orientType;
    const out = [Object.prototype.toString.call(T), T === m.orientType, T.baseVal, T.animVal, SVGMarkerElement.SVG_MARKER_ORIENT_UNKNOWN, SVGMarkerElement.SVG_MARKER_ORIENT_AUTO, SVGMarkerElement.SVG_MARKER_ORIENT_ANGLE];
    m.setAttribute("orient", "auto");
    out.push(T.baseVal, angleOf(A), tryIt(() => { A.value = 30; return m.getAttribute("orient") + " " + T.baseVal; }));
    m.setAttribute("orient", "auto");
    out.push(tryIt(() => { T.baseVal = 2; return m.getAttribute("orient") + " " + A.value; }), tryIt(() => { T.baseVal = 1; return m.getAttribute("orient") + " " + angleOf(A); }), tryIt(() => { T.baseVal = 3; }), tryIt(() => { T.baseVal = 0; }), tryIt(() => { T.animVal = 2; return T.animVal; }));
    m.setAttribute("orient", "45grad");
    out.push(tryIt(() => { T.baseVal = 2; return m.getAttribute("orient"); }), tryIt(() => { T.baseVal = 1; return m.getAttribute("orient"); }), tryIt(() => { T.baseVal = 2; return m.getAttribute("orient"); }));
    m.setAttribute("orient", "auto");
    out.push(tryIt(() => { A.convertToSpecifiedUnits(1); return m.getAttribute("orient") + " " + T.baseVal; }));
    const fresh = svg("marker");
    fresh.setAttribute("orient", "auto");
    out.push(tryIt(() => { fresh.orientAngle.baseVal.valueAsString = "1turn"; }), fresh.getAttribute("orient"), fresh.orientType.baseVal, angleOf(fresh.orientAngle.baseVal));
    m.setAttribute("orient", "auto");
    out.push(tryIt(() => { A.valueAsString = "auto-start-reverse"; return m.getAttribute("orient") + " " + T.baseVal + " " + angleOf(A); }), tryIt(() => { A.valueInSpecifiedUnits = 5; return m.getAttribute("orient") + " " + T.baseVal; }));
    m.setAttribute("orient", "30");
    m.setOrientToAuto();
    out.push(m.getAttribute("orient"), T.baseVal, A.value);
    const a = s.createSVGAngle();
    a.value = 60;
    m.setOrientToAngle(a);
    out.push(m.getAttribute("orient"), T.baseVal, A.value, tryIt(() => { a.value = 70; return m.getAttribute("orient"); }), tryIt(() => m.setOrientToAngle(null)), tryIt(() => m.setOrientToAngle({})), tryIt(() => m.setOrientToAngle()));
    a.newValueSpecifiedUnits(4, 1.5);
    m.setOrientToAngle(a);
    out.push(m.getAttribute("orient"), angleOf(A));
    const other = svg("marker");
    other.setAttribute("orient", "0.25turn");
    m.setOrientToAngle(other.orientAngle.baseVal);
    out.push(m.getAttribute("orient"), angleOf(A));
    other.setAttribute("orient", "auto");
    m.setOrientToAngle(other.orientAngle.baseVal);
    out.push(m.getAttribute("orient"), T.baseVal);
    m.setAttribute("orient", "auto-start-reverse");
    out.push(T.baseVal, T.animVal, angleOf(A));
    const o = new MutationObserver(() => {});
    o.observe(m, { attributes: true });
    m.setOrientToAuto();
    const afterAuto = o.takeRecords().length;
    m.setOrientToAngle(a);
    const afterAngle = o.takeRecords().length;
    T.baseVal = 1;
    A.value = 3;
    out.push(afterAuto, afterAngle, o.takeRecords().length, m.getAttribute("orient"));
    m.removeAttribute("orient");
    out.push(T.baseVal, angleOf(A));
    return out.join(" | ");
  });
  const pointsOf = (L) => L.length + ":" + listOf(L, (p) => p.x + "," + p.y);
  run("svg point list", () => {
    const s = svg("svg");
    const p = svg("polyline");
    const L = p.points;
    const out = [Object.prototype.toString.call(L), L === p.points, p.animatedPoints !== L, p.animatedPoints === p.animatedPoints, L.length, L.numberOfItems, Object.prototype.toString.call(svg("polygon").points), Object.prototype.toString.call(svg("polygon").animatedPoints)];
    for (const value of ["1 2 3 4", "1,2 3,4", "1,2,3,4", "1 2 3", " 1 2 ", "1 2 bad 4", "", "1e2,-.5", "1 2,", "1,,2", "1-2-3-4", "1.5.5 2", ",1 2", "1 2 ,3 4", "1 2\f3 4"]) { p.setAttribute("points", value); out.push(JSON.stringify(value) + "=" + pointsOf(L) + "/" + p.animatedPoints.length + ":" + JSON.stringify(p.getAttribute("points"))); }
    p.setAttribute("points", "1 2 3 4");
    const pt = L.getItem(0);
    out.push(Object.prototype.toString.call(pt), pt !== L.getItem(0), tryIt(() => { pt.x = 9; return p.getAttribute("points"); }), tryIt(() => { pt.y = 1 / 3; return p.getAttribute("points"); }), tryIt(() => { pt.x = NaN; }), L[0].x, tryIt(() => L[5]), tryIt(() => L.getItem(5)));
    const np = s.createSVGPoint();
    np.x = 5;
    np.y = 6;
    out.push(tryIt(() => L.appendItem(np) === np), p.getAttribute("points"), tryIt(() => { np.x = 7; return p.getAttribute("points"); }));
    const q = svg("polygon");
    q.setAttribute("points", "0 0");
    out.push(tryIt(() => { q.points.appendItem(L.getItem(0)); return q.getAttribute("points"); }), tryIt(() => { L.getItem(0).x = 8; return p.getAttribute("points") + " / " + q.getAttribute("points"); }));
    out.push(tryIt(() => L.insertItemBefore(s.createSVGPoint(), 1).x), p.getAttribute("points"), tryIt(() => L.replaceItem(s.createSVGPoint(), 9)), tryIt(() => L.removeItem(0).x), p.getAttribute("points"), tryIt(() => { L[0] = s.createSVGPoint(); return p.getAttribute("points"); }), tryIt(() => { L[0] = 5; }), tryIt(() => L.initialize(np).y), p.getAttribute("points"));
    const removed = L.removeItem(0);
    out.push(JSON.stringify(p.getAttribute("points")), tryIt(() => { removed.x = 1; return JSON.stringify(p.getAttribute("points")); }));
    p.setAttribute("points", "1 2");
    out.push(tryIt(() => p.animatedPoints.clear()), tryIt(() => { p.animatedPoints.getItem(0).x = 3; }), tryIt(() => p.animatedPoints.appendItem(np)), tryIt(() => { p.animatedPoints[0] = np; }), pointsOf(p.animatedPoints));
    const o = new MutationObserver(() => {});
    o.observe(p, { attributes: true });
    L.appendItem(s.createSVGPoint());
    L.getItem(0).y = 4;
    out.push(o.takeRecords().length, p.getAttribute("points"));
    out.push(Object.prototype.toString.call(np), [s.createSVGPoint().x, s.createSVGPoint().y].join(), s.createSVGPoint() !== s.createSVGPoint(), tryIt(() => new SVGPoint()), typeof np.matrixTransform, tryIt(() => { L.clear(); return JSON.stringify(p.getAttribute("points")); }));
    return out.join(" | ");
  });
  // Twelve digits, as rotations go through cos and sin, which a platform's
  // math library may round a last bit apart.
  const num12 = (x) => String(+x.toPrecision(12));
  const matrixOf = (m) => [m.a, m.b, m.c, m.d, m.e, m.f].map(num12).join(",");
  const transformOf = (t) => t.type + "/" + num12(t.angle) + "/" + matrixOf(t.matrix);
  const transformsOf = (L) => L.length + ":" + listOf(L, transformOf);
  run("svg transform list parse", () => {
    const g = svg("g");
    const L = g.transform.baseVal;
    const out = [Object.prototype.toString.call(g.transform), Object.prototype.toString.call(L), L === g.transform.baseVal, g.transform.animVal !== L, L.length, L.numberOfItems, SVGTransform.SVG_TRANSFORM_UNKNOWN, SVGTransform.SVG_TRANSFORM_SKEWY];
    for (const value of ["translate(10)", "translate(10, 20)", "translate(10 20)", "translate(10,)", "translate( 10 , 20 )", "scale(2)", "scale(2,3)", "rotate(45)", "rotate(45 10 20)", "rotate(45, 10)", "rotate(30,1,2,3)", "skewX(30)", "skewY(-30)", "matrix(1 2 3 4 5 6)", "matrix(1,2,3,4,5)", "translate(1) scale(2)", "translate(1),scale(2)", "translate(1) , scale(2)", "translate(1),", "translate (1)", "Translate(1)", "translate(1)scale(2)", "", "   ", "rotate(90)", "rotate(180 5 5)", "rotate(-90 3 4)", "skewX(90)", "scale(1e2 -.5)", "translate(1e40)", "matrix(1 0 0 1 0 0) bad", "translate(1)\tscale(2)\n", "translate()", "skew(10)", "translate(1 2 3)"]) {
      g.setAttribute("transform", value);
      let text = "-";
      if (L.length) { L.replaceItem(L.getItem(0), 0); text = g.getAttribute("transform"); }
      out.push(JSON.stringify(value) + "=" + transformsOf(L) + "=" + JSON.stringify(text) + "/" + g.transform.animVal.length);
    }
    return out.join(" | ");
  });
  run("svg transform objects", () => {
    const s = svg("svg");
    const t = s.createSVGTransform();
    const out = [Object.prototype.toString.call(t), transformOf(t), t.matrix === t.matrix, Object.prototype.toString.call(t.matrix), t !== s.createSVGTransform(), tryIt(() => new SVGTransform())];
    t.setTranslate(1, 2);
    out.push(transformOf(t));
    t.setScale(2, -3);
    out.push(transformOf(t), Object.is(t.matrix.b, -0), Object.is(t.matrix.c, -0));
    t.setRotate(30, 5, 6);
    out.push(transformOf(t));
    t.setSkewX(45);
    out.push(transformOf(t));
    t.setSkewY(-45);
    out.push(transformOf(t));
    const m = s.createSVGMatrix();
    m.a = 2;
    m.f = 7;
    t.setMatrix(m);
    m.a = 3;
    out.push(transformOf(t), tryIt(() => t.setMatrix(null)), tryIt(() => t.setMatrix({})), tryIt(() => t.setTranslate(NaN, 1)), tryIt(() => t.setRotate(1, 2)));
    t.setRotate(90, 0, 0);
    const tm = t.matrix;
    tm.e = 4;
    out.push(transformOf(t), tm === t.matrix);
    const fm = s.createSVGTransformFromMatrix(m);
    m.b = 9;
    out.push(transformOf(fm), tryIt(() => s.createSVGTransformFromMatrix(null)));
    t.setSkewX(0.1);
    out.push(transformOf(t));
    return out.join(" | ");
  });
  run("svg transform list operations", () => {
    const s = svg("svg");
    const g = svg("g");
    const L = g.transform.baseVal;
    g.setAttribute("transform", "translate(1 2) scale(3)");
    const t0 = L.getItem(0);
    const out = [t0 !== L.getItem(0), tryIt(() => { t0.setRotate(10, 0, 0); return g.getAttribute("transform"); }), tryIt(() => { t0.matrix.a = 2; return g.getAttribute("transform") + " " + transformOf(L.getItem(0)); }), tryIt(() => L.getItem(5)), tryIt(() => L[5]), L[1].type];
    const created = s.createSVGTransform();
    created.setScale(4, 4);
    out.push(L.appendItem(created) === created, g.getAttribute("transform"), tryIt(() => { created.setTranslate(9, 9); return g.getAttribute("transform"); }), tryIt(() => { created.matrix.f = 5; return g.getAttribute("transform"); }));
    const h = svg("rect");
    h.setAttribute("transform", "skewX(10)");
    out.push(tryIt(() => { h.transform.baseVal.appendItem(L.getItem(0)); return h.getAttribute("transform"); }), tryIt(() => { L.getItem(0).setScale(5, 5); return g.getAttribute("transform") + " / " + h.getAttribute("transform"); }));
    out.push(tryIt(() => L.insertItemBefore(s.createSVGTransform(), 1).type), g.getAttribute("transform"), tryIt(() => L.replaceItem(s.createSVGTransform(), 9)), tryIt(() => L.removeItem(0).type), g.getAttribute("transform"), tryIt(() => { L[0] = s.createSVGTransform(); return g.getAttribute("transform"); }), tryIt(() => { L[0] = 5; }), tryIt(() => L.initialize(created).type), g.getAttribute("transform"));
    const removed = L.removeItem(0);
    out.push(JSON.stringify(g.getAttribute("transform")), tryIt(() => { removed.setScale(2, 2); return JSON.stringify(g.getAttribute("transform")) + " " + transformOf(removed); }));
    g.setAttribute("transform", "rotate(90) translate(10 0) scale(2)");
    const items = [L.getItem(0), L.getItem(1)];
    const c = L.consolidate();
    out.push(Object.prototype.toString.call(c), transformOf(c), L.length, g.getAttribute("transform"), items.map(transformOf).join(";"), tryIt(() => { c.matrix.e = 1; return g.getAttribute("transform"); }), tryIt(() => { items[0].setScale(7, 7); return g.getAttribute("transform"); }));
    g.removeAttribute("transform");
    out.push(String(L.consolidate()), tryIt(() => g.transform.animVal.consolidate()));
    g.setAttribute("transform", "translate(1)");
    out.push(tryIt(() => g.transform.animVal.clear()), tryIt(() => { g.transform.animVal.getItem(0).setScale(1, 1); }), tryIt(() => { g.transform.animVal.getItem(0).matrix.a = 2; }), tryIt(() => { g.transform.animVal[0] = created; }), transformsOf(g.transform.animVal));
    out.push(tryIt(() => transformOf(L.createSVGTransformFromMatrix(s.createSVGMatrix().translate(3, 4)))), tryIt(() => transformOf(g.transform.animVal.createSVGTransformFromMatrix(s.createSVGMatrix()))), L.length);
    const o = new MutationObserver(() => {});
    o.observe(g, { attributes: true });
    L.appendItem(s.createSVGTransform());
    L.getItem(0).setSkewY(5);
    L.getItem(1).matrix.d = 3;
    out.push(o.takeRecords().length, g.getAttribute("transform"));
    out.push(tryIt(() => { L.clear(); return JSON.stringify(g.getAttribute("transform")); }));
    return out.join(" | ");
  });
  run("svg matrix", () => {
    const s = svg("svg");
    const m = s.createSVGMatrix();
    const out = [Object.prototype.toString.call(m), matrixOf(m), m !== s.createSVGMatrix(), tryIt(() => new SVGMatrix())];
    m.a = 2; m.b = 0.5; m.c = -1; m.d = 3; m.e = 10; m.f = -20;
    out.push(matrixOf(m), tryIt(() => { m.a = NaN; }), tryIt(() => { m.e = Infinity; }));
    const other = s.createSVGMatrix().scaleNonUniform(2, 4).translate(1, 1);
    out.push(matrixOf(m.translate(3, 4)), matrixOf(m.scale(2)), matrixOf(m.scaleNonUniform(2, 0.5)), matrixOf(m.rotate(30)), matrixOf(m.rotate(90)), matrixOf(m.flipX()), matrixOf(m.flipY()), matrixOf(m.skewX(30)), matrixOf(m.skewY(-45)), matrixOf(m.multiply(other)), matrixOf(other.multiply(m)), matrixOf(m.inverse()), matrixOf(m.rotateFromVector(1, 1)), matrixOf(m.rotateFromVector(-3, 4)), tryIt(() => m.rotateFromVector(0, 1)), tryIt(() => m.rotateFromVector(1, 0)), tryIt(() => m.multiply(null)), m.translate(1, 1) !== m, matrixOf(m));
    const singular = s.createSVGMatrix();
    singular.a = 0;
    out.push(tryIt(() => singular.inverse()), tryIt(() => { singular.a = 1; singular.d = 1e-320; return matrixOf(singular.inverse()); }), tryIt(() => { const t = s.createSVGMatrix(); t.e = 5; t.f = -6; return matrixOf(t.inverse()); }));
    out.push(matrixOf(s.createSVGMatrix().translate(0.1, 0.2)), matrixOf(s.createSVGMatrix().rotate(0.1)), matrixOf(s.createSVGMatrix().skewX(1e-3)));
    const p = s.createSVGPoint();
    p.x = 3;
    p.y = 4;
    const q = p.matrixTransform(m);
    out.push(Object.prototype.toString.call(q), q !== p, q.x + "," + q.y, p.x + "," + p.y, tryIt(() => p.matrixTransform(null)), tryIt(() => { const big = s.createSVGMatrix(); big.a = 1e300; return p.matrixTransform(big).x; }), tryIt(() => { const n = s.createSVGMatrix(); n.a = 1e300; n.c = -1e300; p.x = 1; p.y = 1; return p.matrixTransform(n).x; }));
    const poly = svg("polyline");
    poly.setAttribute("points", "1 2");
    const moved = poly.points.getItem(0).matrixTransform(s.createSVGMatrix().translate(5, 5));
    out.push(moved.x + "," + moved.y, tryIt(() => { moved.x = 0; return poly.getAttribute("points"); }));
    return out.join(" | ");
  });
  run("svg transform owners", () => ["g.transform", "rect.transform", "svg.transform", "text.transform", "use.transform", "a.transform", "symbol.transform", "clipPath.transform", "mask.transform", "marker.transform", "linearGradient.gradientTransform", "radialGradient.gradientTransform", "pattern.patternTransform", "pattern.transform", "stop.transform"].map((p) => { const [t, m] = p.split("."); const e = svg(t); return p + "=" + (e[m] === undefined ? "none" : Object.prototype.toString.call(e[m]) + "/" + e[m].baseVal.length); }).join(" | "));
  return JSON.stringify(cases, null, 1);
})()
