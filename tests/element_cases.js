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
  return JSON.stringify(cases, null, 1);
})()
