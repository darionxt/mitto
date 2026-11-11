// What the Runtime's tests compare with Chrome: the own properties of the
// interfaces that the Runtime implements, with their attributes, names and
// lengths, in order; what a call with the wrong receiver throws; and what some
// calls throw or do. It runs as it is in the Page program and, through
// record.html, in Chrome, and its value is the record as JSON.
//
// It records interfaces, descriptors, lengths and messages only, which say
// nothing of the machine that runs it.

(() => {
  const interfaces = [
    "CharacterData",
    "Comment",
    "DOMException",
    "Document",
    "DocumentType",
    "Element",
    "Event",
    "EventTarget",
    "HTMLBRElement",
    "HTMLBodyElement",
    "HTMLDivElement",
    "HTMLDocument",
    "HTMLElement",
    "HTMLHeadElement",
    "HTMLHeadingElement",
    "HTMLHtmlElement",
    "HTMLLIElement",
    "HTMLLinkElement",
    "HTMLMetaElement",
    "HTMLOListElement",
    "HTMLParagraphElement",
    "HTMLPreElement",
    "HTMLScriptElement",
    "HTMLSpanElement",
    "HTMLStyleElement",
    "HTMLTitleElement",
    "HTMLUListElement",
    "HTMLUnknownElement",
    "Node",
    "PopStateEvent",
    "Text",
    "Window",
  ];

  const describeFunction = (f) => f && { name: f.name, length: f.length };

  const describe = (object, name) => {
    const descriptor = Object.getOwnPropertyDescriptor(object, name);
    const result = {
      enumerable: descriptor.enumerable,
      configurable: descriptor.configurable,
    };
    if ("value" in descriptor) {
      result.writable = descriptor.writable;
      const value = descriptor.value;
      if (typeof value === "function") {
        result.value = describeFunction(value);
      } else if (typeof value === "object" && value !== null) {
        result.value = "object";
      } else {
        result.value = value;
      }
    } else {
      result.get = describeFunction(descriptor.get) || null;
      result.set = describeFunction(descriptor.set) || null;
    }
    return result;
  };

  // A list of [name, descriptor], which keeps the order as JSON's objects
  // would not for names that look like numbers.
  const ownProperties = (object) =>
    Object.getOwnPropertyNames(object).map((name) => [name, describe(object, name)]);

  // JSON would write undefined in an array as null, so it is written as a
  // string.
  const json = (value) => JSON.stringify(value, (key, item) => (item === undefined ? "undefined" : item));

  const outcome = (f) => {
    try {
      const value = f();
      return { value: typeof value === "object" && value !== null ? json(value) : String(value) };
    } catch (error) {
      return { threw: String(error) };
    }
  };

  // What each member throws when it is called on an object that is not one of
  // the interface's.
  const wrongReceiver = (prototype) => {
    const result = [];
    for (const name of Object.getOwnPropertyNames(prototype)) {
      const descriptor = Object.getOwnPropertyDescriptor(prototype, name);
      if (name === "constructor") {
        continue;
      }
      if (typeof descriptor.value === "function") {
        result.push([name, outcome(() => descriptor.value.call({}))]);
      }
      if (descriptor.get) {
        result.push([`get ${name}`, outcome(() => descriptor.get.call({}))]);
      }
      if (descriptor.set) {
        result.push([`set ${name}`, outcome(() => descriptor.set.call({}, undefined))]);
      }
    }
    return result;
  };

  // What the DOM does, on elements and documents that the case makes, never on
  // the page's own document, which is another in Chrome's page.
  const domCases = () => {
    const div = (markup) => {
      const element = document.createElement("div");
      element.innerHTML = markup;
      return element;
    };
    const tagOf = (node) => Object.prototype.toString.call(node);
    const tree = (node) => {
      // The tree under a node, written as html5lib's tests write it.
      const lines = [];
      const walk = (current, depth) => {
        for (let child = current.firstChild; child; child = child.nextSibling) {
          const indent = "  ".repeat(depth);
          if (child.nodeType === Node.ELEMENT_NODE) {
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
      walk(node, 0);
      return lines;
    };
    const parsed = (context, markup) => {
      const element = document.createElement(context);
      element.innerHTML = markup;
      return tree(element);
    };
    return {
      "the tree of a document's markup in the html element": () =>
        parsed("html", "<!-- a --><title> T  </title><meta charset=utf-8>x<p>One<p>Two</p><!--b--></body>after"),
      "the tree of markup that leaves out head and body": () => parsed("html", "text<div>a</div>"),
      "the tree of markup in a head": () => parsed("head", "<title>t</title> <link rel=x>y"),
      "implied end tags": () =>
        parsed("div", "<p>a<div>b</div><ul><li>1<li>2</ul><dl><dt>t<dd>d<dt>u</dl><h1>h<h2>i</h2><p>c<h3>j"),
      "end tags that close what is open": () => parsed("div", "<div><span>a</div>b</span><p>c</p></p><br></br>d"),
      "void elements": () => parsed("div", "<br><img src=a><hr><input type=text><wbr><area><embed><source>x<image>"),
      "raw text and RCDATA": () =>
        parsed("div", "<style>a<b>&amp;</style><textarea>\n<b>&amp;</textarea><title>&lt;x</title><xmp>&lt;</xmp>"),
      "scripts' text": () => parsed("div", "<script>if (a < b && c) { x = '</div>' }</script>after"),
      "pre drops its first line feed": () => parsed("div", "<pre>\n\nx</pre><listing>\ny</listing>"),
      "attributes as they come": () => parsed("div", "<a HREF=x data-a='1' data-A=2 b c=\"d e\">t</a>"),
      "character references": () => parsed("div", "&amp;&lt;&gt;&quot;&nbsp;&copy;&#x41;&#66;&notin;&notit;&bogus;"),
      "innerHTML writes markup back": () =>
        div("<p class=\"a b\" data-x='&quot;&amp;<>'>One &amp; two &lt; three &gt; four\u00a0</p><!--c--><br>" +
          "<script>a < b</script><style>&</style><span>x</span>").innerHTML,
      "outerHTML": () => {
        const element = div("<b>x</b>");
        return [element.outerHTML, element.firstChild.outerHTML];
      },
      "outerHTML's setter": () => {
        const element = div("<i>a</i><b>b</b><u>c</u>");
        element.firstChild.nextSibling.outerHTML = "<em>1</em>2";
        const lone = document.createElement("p");
        lone.outerHTML = "<b>x</b>";
        const doc = new Document();
        const root = doc.createElement("root");
        doc.appendChild(root);
        let thrown;
        try {
          root.outerHTML = "<x>";
        } catch (error) {
          thrown = [error.name, error.message];
        }
        return [element.innerHTML, lone.outerHTML, thrown];
      },
      "textContent": () => {
        const element = div("<p>a<b>b</b><!--c-->c</p>");
        const before = element.textContent;
        element.firstChild.textContent = "x<y>";
        const after = element.innerHTML;
        element.textContent = null;
        return [before, after, element.innerHTML, element.firstChild, new Text("t").textContent, document.textContent];
      },
      "nodeValue": () => {
        const element = div("x<!--y--><b>z</b>");
        element.lastChild.nodeValue = "ignored";
        element.firstChild.nodeValue = null;
        return [element.nodeValue, element.innerHTML];
      },
      "querySelector with simple selectors": () => {
        const root = div(
          "<p id=one class='a b'>1</p><p id=two class=b lang=en-US>2<span id=three data-x='y z' title=Hello>3</span></p>" +
            "<div id=four><p id=five>5</p></div><a id=six href='http://example.org/x.html' type=TEXT>6</a>",
        );
        const selectors = [
          "#two", ".a", "p.b", ".b.a", "div > p", "div p span", "p + p", "#one ~ p", "p ~ a", "[data-x]",
          "[data-x='y z']", "[data-x~=z]", "[data-x~='y z']", "[lang|=en]", "[lang|=US]", "[href^=http]",
          "[href$='.html']", "[href*=example]", "[title=hello i]", "[title=hello]", "[type=text]", "[type=text s]",
          "*", "P", "SPAN#three", "p, a", "#nothing", "div#four > p#five", "[id=\\36]", "#\\31", "x, #six",
        ];
        return selectors.map((selector) => {
          const found = root.querySelector(selector);
          return [selector, found ? found.id : null];
        });
      },
      "selectors that are not valid": () =>
        ["", "div[", "#1", "..a", ">p", "p >", "[a=]", "a,", "[a=b x]", "p!"].map((selector) => {
          try {
            div("<p>").querySelector(selector);
            return [selector, "valid"];
          } catch (error) {
            return [selector, error.name, error.message];
          }
        }),
      "matches and closest": () => {
        const root = div("<section id=s class=c><p id=p><b id=b>x</b></p></section>");
        const b = root.querySelector("#b");
        return [
          b.matches("p > b"),
          b.matches("section b"),
          b.matches("div > b"),
          b.webkitMatchesSelector("b, i"),
          b.closest("section").id,
          b.closest("b").id,
          b.closest("table"),
          b.closest(".c > p").id,
        ];
      },
      "getElementById and a document's parts": () => {
        const doc = new Document();
        const root = doc.createElement("Root");
        doc.appendChild(root);
        const a = doc.createElement("a");
        a.setAttribute("id", "x");
        root.appendChild(a);
        return [
          doc.getElementById("x") === a,
          doc.getElementById(""),
          doc.getElementById("y"),
          doc.documentElement === root,
          root.tagName,
          root.localName,
          root.namespaceURI,
          root.prefix,
          doc.compatMode,
          doc.contentType,
          doc.characterSet,
          doc.charset,
          doc.inputEncoding,
          doc.URL,
          doc.documentURI,
          doc.doctype,
          doc.body,
          doc.head,
          doc.title,
          doc.defaultView,
          doc.ownerDocument,
          a.ownerDocument === doc,
          doc.nodeName,
          doc.nodeType,
          doc.textContent,
          doc.childElementCount,
          tagOf(doc),
        ];
      },
      "createElement": () =>
        ["DIV", "Div", "x-custom", "unknown", "acronym", "image", "keygen", "listing", "h6", "bdi", "svg", "math"].map(
          (name) => {
            const element = document.createElement(name);
            return [name, element.tagName, element.localName, element.namespaceURI, tagOf(element)];
          },
        ),
      "createElement with names that are not valid": () =>
        ["", "1a", "a b", "a>", "-a", "\u00e9t\u00e9", ":a", "a\u0000"].map((name) => {
          try {
            return [name, document.createElement(name).localName];
          } catch (error) {
            return [name, error.name, error.message];
          }
        }),
      "attributes": () => {
        const element = document.createElement("div");
        element.setAttribute("Data-A", "1");
        element.setAttribute("b", "2");
        element.setAttribute("data-a", "3");
        const names = element.getAttributeNames();
        const values = [element.getAttribute("DATA-A"), element.getAttribute("c"), element.hasAttribute("B")];
        element.removeAttribute("B");
        const toggles = [
          element.toggleAttribute("t"),
          element.hasAttribute("t"),
          element.toggleAttribute("t"),
          element.toggleAttribute("t", false),
          element.toggleAttribute("t", true),
          element.toggleAttribute("t", true),
          element.getAttribute("t"),
        ];
        element.id = "i";
        element.className = "c d";
        element.slot = "s";
        const errors = ["", "a b", "a=b", "a/b", "a>b"].map((name) => {
          try {
            element.setAttribute(name, "x");
            return [name, "set"];
          } catch (error) {
            return [name, error.name, error.message];
          }
        });
        return [names, values, element.hasAttributes(), toggles, element.outerHTML, element.id, errors];
      },
      "reflected attributes": () => {
        const meta = document.createElement("meta");
        meta.name = "n";
        meta.content = "c";
        meta.httpEquiv = "h";
        const script = document.createElement("script");
        script.type = "module";
        script.defer = true;
        script.noModule = true;
        script.noModule = false;
        script.text = "a < b";
        const ol = document.createElement("ol");
        ol.reversed = true;
        const html = document.createElement("html");
        html.version = "v";
        const title = document.createElement("title");
        title.text = "x";
        return [meta.outerHTML, script.outerHTML, script.text, ol.outerHTML, html.outerHTML, title.outerHTML, title.text];
      },
      "tree mutations": () => {
        const root = div("<p id=a>a</p><p id=b>b</p>");
        const a = root.firstChild;
        const b = root.lastChild;
        const c = document.createElement("i");
        const results = [
          root.appendChild(c) === c,
          root.insertBefore(b, a) === b,
          root.insertBefore(a, null) === a,
          root.insertBefore(c, c) === c,
          root.replaceChild(document.createTextNode("t"), c) === c,
          root.removeChild(a) === a,
          root.innerHTML,
        ];
        const attempts = {
          "itself": () => root.appendChild(root),
          "an ancestor": () => root.firstChild.appendChild(root),
          "into a text": () => document.createTextNode("x").appendChild(c),
          "remove from a text": () => document.createTextNode("x").removeChild(c),
          "remove another's child": () => root.removeChild(c),
          "before another's child": () => root.insertBefore(c, a),
          "replace another's child": () => root.replaceChild(c, a),
          "a document": () => root.appendChild(new Document()),
          "a text into a document": () => new Document().appendChild(document.createTextNode("x")),
          "two elements into a document": () => {
            const doc = new Document();
            doc.appendChild(doc.createElement("a"));
            doc.appendChild(doc.createElement("b"));
          },
          "an element before nothing": () => root.insertBefore(c),
          "null": () => root.appendChild(null),
          "an object": () => root.appendChild({}),
        };
        for (const [name, attempt] of Object.entries(attempts)) {
          try {
            attempt();
            results.push([name, "done"]);
          } catch (error) {
            results.push([name, error.name, error.message]);
          }
        }
        return results;
      },
      "append, prepend, before, after, replaceWith and remove": () => {
        const root = div("<i>1</i><b>2</b>");
        const i = root.firstChild;
        const b = root.lastChild;
        root.append("x", document.createElement("u"), "y");
        root.prepend(b, "p");
        i.before("bf", b);
        i.after("af");
        i.replaceWith("r", i, "s");
        const steps = [root.innerHTML];
        b.remove();
        root.replaceChildren("only", i);
        steps.push(root.innerHTML);
        root.replaceChildren();
        steps.push(root.innerHTML, root.childElementCount);
        const lone = document.createElement("q");
        lone.before("nothing");
        lone.after("nothing");
        lone.replaceWith("nothing");
        lone.remove();
        steps.push(lone.parentNode);
        try {
          root.append(root);
        } catch (error) {
          steps.push([error.name, error.message]);
        }
        return steps;
      },
      "element children and siblings": () => {
        const root = div("a<i>1</i>b<b>2</b>c<u>3</u>d");
        const b = root.querySelector("b");
        return [
          root.firstElementChild.localName,
          root.lastElementChild.localName,
          root.childElementCount,
          b.previousElementSibling.localName,
          b.nextElementSibling.localName,
          root.firstChild.nextElementSibling.localName,
          root.lastChild.previousElementSibling.localName,
          root.firstElementChild.previousElementSibling,
          root.hasChildNodes(),
          b.firstChild.hasChildNodes(),
        ];
      },
      "insertAdjacentElement and insertAdjacentText": () => {
        const root = div("<p>x</p>");
        const p = root.firstChild;
        p.insertAdjacentElement("beforebegin", document.createElement("a"));
        p.insertAdjacentElement("AfterBegin", document.createElement("b"));
        p.insertAdjacentText("beforeEnd", "t");
        p.insertAdjacentText("afterend", "u");
        const lone = document.createElement("s");
        const results = [root.innerHTML, lone.insertAdjacentElement("beforebegin", document.createElement("c"))];
        try {
          p.insertAdjacentText("middle", "x");
        } catch (error) {
          results.push([error.name, error.message]);
        }
        return results;
      },
      "CharacterData": () => {
        const text = new Text("hello");
        const steps = [text.length, text.substringData(1, 3), text.substringData(2, 100)];
        text.appendData(" world");
        text.insertData(0, ">");
        text.deleteData(1, 1);
        text.replaceData(0, 1, "<<");
        steps.push(text.data, new Comment("c").data, new Text().data, new Comment().nodeName);
        for (const call of [
          () => text.substringData(100, 1),
          () => text.insertData(100, "x"),
          () => text.deleteData(100, 1),
          () => text.replaceData(100, 1, "x"),
        ]) {
          try {
            call();
          } catch (error) {
            steps.push([error.name, error.message]);
          }
        }
        text.data = null;
        steps.push(text.data);
        return steps;
      },
      "splitText, wholeText and normalize": () => {
        const root = div("");
        const text = new Text("abcdef");
        root.append(text);
        const rest = text.splitText(2);
        const steps = [text.data, rest.data, root.childElementCount, text.nextSibling === rest, text.wholeText];
        root.append("", "g");
        root.normalize();
        steps.push(root.firstChild.data, root.firstChild === text, root.firstChild.nextSibling);
        try {
          text.splitText(100);
        } catch (error) {
          steps.push([error.name, error.message]);
        }
        return steps;
      },
      "cloneNode and isEqualNode": () => {
        const root = div("<p a=1>x<b>y</b><!--z--></p>");
        const p = root.firstChild;
        const shallow = p.cloneNode();
        const deep = p.cloneNode(true);
        return [
          shallow.outerHTML,
          deep.outerHTML,
          deep.isEqualNode(p),
          shallow.isEqualNode(p),
          p.isEqualNode(null),
          p.isSameNode(p),
          deep.isSameNode(p),
          deep.parentNode,
          new Document().cloneNode(true).nodeName,
          div("<p a=1 b=2>").firstChild.isEqualNode(div("<p b=2 a=1>").firstChild),
        ];
      },
      "compareDocumentPosition and contains": () => {
        const root = div("<p><b>1</b><i>2</i></p><u>3</u>");
        const p = root.firstChild;
        const b = p.firstChild;
        const i = p.lastChild;
        const u = root.lastChild;
        return [
          b.compareDocumentPosition(i),
          i.compareDocumentPosition(b),
          p.compareDocumentPosition(b),
          b.compareDocumentPosition(p),
          b.compareDocumentPosition(u),
          b.compareDocumentPosition(b),
          root.contains(b),
          b.contains(root),
          b.contains(b),
          b.contains(null),
        ];
      },
      "a node's place": () => {
        const root = div("<p><b>x</b></p>");
        const b = root.querySelector("b");
        const text = b.firstChild;
        return [
          b.getRootNode() === root,
          b.getRootNode({ composed: true }) === root,
          b.isConnected,
          document.isConnected,
          b.parentElement.localName,
          root.parentElement,
          text.parentElement === b,
          b.ownerDocument === document,
          text.nodeName,
          text.nodeType,
          b.nodeName,
          b.nodeType,
          new Comment("x").nodeType,
          document.nodeType,
        ];
      },
      "the window": () => [
        window === self,
        window === globalThis,
        window.window === window,
        tagOf(window),
        Object.getPrototypeOf(window) === Window.prototype,
        Object.getPrototypeOf(Object.getPrototypeOf(Window.prototype)) === EventTarget.prototype,
        Object.getOwnPropertyNames(Object.getPrototypeOf(Window.prototype)),
        window instanceof EventTarget,
        document instanceof Document,
        tagOf(document),
        tagOf(new Document()),
        document.defaultView === window,
        document.nodeName,
        document.documentElement instanceof HTMLHtmlElement,
        typeof Object.getOwnPropertyDescriptor(window, "document").get,
        Object.getOwnPropertyDescriptor(window, "document").configurable,
        Object.getOwnPropertyDescriptor(window, "window").configurable,
        Object.getOwnPropertyDescriptor(window, "self").configurable,
        (() => {
          try {
            Object.setPrototypeOf(window, {});
            return "set";
          } catch (error) {
            return [error.name];
          }
        })(),
        (() => {
          try {
            Object.setPrototypeOf(EventTarget.prototype, {});
            return "set";
          } catch (error) {
            return [error.name];
          }
        })(),
      ],
      "self can be replaced": () => {
        const before = self === window;
        self = 5;
        const after = self;
        delete globalThis.self;
        return [before, after, self === window];
      },
      "a node's @@unscopables": () =>
        [Element, Document, DocumentType, CharacterData].map((type) => {
          const unscopables = type.prototype[Symbol.unscopables];
          return [type.name, Object.getPrototypeOf(unscopables), Object.keys(unscopables)];
        }),
      "illegal constructors": () =>
        [Node, Element, HTMLElement, HTMLDivElement, HTMLUnknownElement, CharacterData, DocumentType].map((type) => {
          const results = [type.name];
          for (const call of [() => new type(), () => type()]) {
            try {
              call();
              results.push("made");
            } catch (error) {
              results.push(String(error));
            }
          }
          return results;
        }),
    };
  };

  const record = { interfaces: {}, instances: {}, cases: [] };
  for (const name of interfaces) {
    const object = globalThis[name];
    if (typeof object !== "function") {
      record.interfaces[name] = null;
      continue;
    }
    const parent = Object.getPrototypeOf(object.prototype);
    record.interfaces[name] = {
      global: describe(globalThis, name),
      interfaceObject: ownProperties(object),
      prototype: ownProperties(object.prototype),
      prototypeParent: parent === Object.prototype ? "Object" : parent.constructor.name,
      interfaceObjectParent:
        Object.getPrototypeOf(object) === Function.prototype ? "Function" : Object.getPrototypeOf(object).name,
      toStringTag: object.prototype[Symbol.toStringTag],
      callWithoutNew: outcome(() => object()),
      constructWithoutArguments: outcome(() => new object()),
      wrongReceiver: wrongReceiver(object.prototype),
    };
  }

  // Elements come from a document of their own, which a page's own does not
  // change, and an HTML one from the page's document, as they are the same in
  // Chrome's page and the Page program's.
  const element = (name) => document.createElement(name);
  const instances = {
    Comment: () => new Comment("x"),
    DOMException: () => new DOMException(),
    Document: () => new Document(),
    Element: () => new Document().createElement("x"),
    Event: () => new Event("x"),
    EventTarget: () => new EventTarget(),
    HTMLBRElement: () => element("br"),
    HTMLBodyElement: () => element("body"),
    HTMLDivElement: () => element("div"),
    HTMLDocument: () => document,
    HTMLElement: () => element("section"),
    HTMLHeadElement: () => element("head"),
    HTMLHeadingElement: () => element("h1"),
    HTMLHtmlElement: () => element("html"),
    HTMLLIElement: () => element("li"),
    HTMLLinkElement: () => element("link"),
    HTMLMetaElement: () => element("meta"),
    HTMLOListElement: () => element("ol"),
    HTMLParagraphElement: () => element("p"),
    HTMLPreElement: () => element("pre"),
    HTMLScriptElement: () => element("script"),
    HTMLSpanElement: () => element("span"),
    HTMLStyleElement: () => element("style"),
    HTMLTitleElement: () => element("title"),
    HTMLUListElement: () => element("ul"),
    HTMLUnknownElement: () => element("unknown"),
    PopStateEvent: () => new PopStateEvent("x"),
    Text: () => new Text("x"),
  };
  for (const [name, make] of Object.entries(instances)) {
    record.instances[name] = ownProperties(make());
  }

  const cases = {
    "addEventListener with a number": () => new EventTarget().addEventListener("x", 5),
    "addEventListener with one argument": () => new EventTarget().addEventListener("x"),
    "addEventListener with a signal that is not an AbortSignal": () =>
      new EventTarget().addEventListener("x", () => {}, { signal: 1 }),
    "addEventListener with null": () => new EventTarget().addEventListener("x", null),
    "removeEventListener with a string": () => new EventTarget().removeEventListener("x", "y"),
    "dispatchEvent with an object": () => new EventTarget().dispatchEvent({}),
    "dispatchEvent with null": () => new EventTarget().dispatchEvent(null),
    "dispatchEvent without an argument": () => new EventTarget().dispatchEvent(),
    "dispatchEvent while the event is dispatched": () => {
      const target = new EventTarget();
      const event = new Event("x");
      let thrown;
      target.addEventListener("x", () => {
        try {
          target.dispatchEvent(event);
        } catch (error) {
          thrown = [error.name, error.message, error.code, error instanceof DOMException, error instanceof Error];
        }
      });
      target.dispatchEvent(event);
      return thrown;
    },
    "the own properties of a DOMException that is thrown and one that is made": () => {
      const target = new EventTarget();
      const event = new Event("x");
      let thrown;
      target.addEventListener("x", () => {
        try {
          target.dispatchEvent(event);
        } catch (error) {
          thrown = error;
        }
      });
      target.dispatchEvent(event);
      return [ownProperties(thrown), typeof thrown.stack, ownProperties(new DOMException("m"))];
    },
    "Event with a number for its dictionary": () => new Event("x", 5),
    "Event with a symbol for its type": () => new Event(Symbol("s")),
    "Event with a dictionary whose getter throws": () =>
      new Event("x", {
        get bubbles() {
          throw new RangeError("from the page");
        },
      }),
    "addEventListener with a symbol for its type": () => new EventTarget().addEventListener(Symbol("s"), null),
    "Event with a dictionary": () => {
      const event = new Event("x", { bubbles: 1, cancelable: "", composed: {} });
      return [event.type, event.bubbles, event.cancelable, event.composed, event.isTrusted, event.eventPhase];
    },
    "Event's flags before dispatch": () => {
      const event = new Event("x", { cancelable: true });
      event.preventDefault();
      return [event.defaultPrevented, event.returnValue, event.target, event.currentTarget, event.composedPath().length];
    },
    "dispatch to listeners": () => {
      const target = new EventTarget();
      const calls = [];
      const event = new Event("x", { cancelable: true });
      target.addEventListener("x", function (e) {
        calls.push(["bubbling", e.eventPhase, this === target, e.currentTarget === target, e.target === target, e === event]);
        e.preventDefault();
      });
      target.addEventListener(
        "x",
        (e) => calls.push(["capturing", e.eventPhase, e.composedPath().length, e.composedPath()[0] === target]),
        true,
      );
      target.addEventListener("y", () => calls.push(["other type"]));
      const result = target.dispatchEvent(event);
      calls.push([result, event.defaultPrevented, event.eventPhase, event.currentTarget, event.target === target]);
      return calls;
    },
    "dispatchEvent of a canceled event": () => {
      const canceled = () => {
        const event = new Event("x", { cancelable: true });
        event.preventDefault();
        return event;
      };
      const removed = new EventTarget();
      const listener = () => {};
      removed.addEventListener("x", listener);
      removed.removeEventListener("x", listener);
      const otherType = new EventTarget();
      otherType.addEventListener("y", listener);
      const withNull = new EventTarget();
      withNull.addEventListener("x", null);
      return [
        new EventTarget().dispatchEvent(canceled()),
        removed.dispatchEvent(canceled()),
        otherType.dispatchEvent(canceled()),
        withNull.dispatchEvent(canceled()),
      ];
    },
    "a listener added twice runs once": () => {
      const target = new EventTarget();
      let count = 0;
      const listener = () => count++;
      target.addEventListener("x", listener);
      target.addEventListener("x", listener, { capture: false });
      target.dispatchEvent(new Event("x"));
      return count;
    },
    "once": () => {
      const target = new EventTarget();
      let count = 0;
      target.addEventListener("x", () => count++, { once: true });
      target.dispatchEvent(new Event("x"));
      target.dispatchEvent(new Event("x"));
      return count;
    },
    "removeEventListener": () => {
      const target = new EventTarget();
      let count = 0;
      const listener = () => count++;
      target.addEventListener("x", listener, true);
      target.removeEventListener("x", listener);
      target.dispatchEvent(new Event("x"));
      target.removeEventListener("x", listener, { capture: true });
      target.dispatchEvent(new Event("x"));
      return count;
    },
    "handleEvent": () => {
      const target = new EventTarget();
      const listener = { calls: 0, handleEvent(e) { this.calls++; } };
      target.addEventListener("x", listener);
      target.dispatchEvent(new Event("x"));
      return listener.calls;
    },
    "stopImmediatePropagation": () => {
      const target = new EventTarget();
      const calls = [];
      target.addEventListener("x", (e) => { calls.push(1); e.stopImmediatePropagation(); });
      target.addEventListener("x", () => calls.push(2));
      const event = new Event("x");
      target.dispatchEvent(event);
      return [calls, event.cancelBubble];
    },
    "stopPropagation in a capturing listener": () => {
      const target = new EventTarget();
      const calls = [];
      target.addEventListener("x", () => calls.push("bubbling"));
      target.addEventListener("x", (e) => { calls.push("capturing"); e.stopPropagation(); }, true);
      target.dispatchEvent(new Event("x"));
      return calls;
    },
    "preventDefault in a passive listener": () => {
      const target = new EventTarget();
      target.addEventListener("x", (e) => e.preventDefault(), { passive: true });
      const event = new Event("x", { cancelable: true });
      return [target.dispatchEvent(event), event.defaultPrevented];
    },
    "initEvent": () => {
      const event = new Event("x");
      event.initEvent("y", true, true);
      return [event.type, event.bubbles, event.cancelable];
    },
    "returnValue and cancelBubble": () => {
      const event = new Event("x", { cancelable: true });
      event.returnValue = false;
      event.cancelBubble = true;
      return [event.defaultPrevented, event.returnValue, event.cancelBubble];
    },
    "DOMException's defaults": () => {
      const exception = new DOMException();
      return [exception.name, exception.message, exception.code, String(exception)];
    },
    "DOMException with a name": () => {
      const exception = new DOMException("m", "SyntaxError");
      return [exception.name, exception.message, exception.code, exception instanceof Error];
    },
    "PopStateEvent with a state": () => {
      const state = {};
      const event = new PopStateEvent("p", { state, hasUAVisualTransition: true, bubbles: true });
      const plain = new PopStateEvent("q");
      return [event.state === state, event.hasUAVisualTransition, event.bubbles, plain.state === null, plain.bubbles];
    },
    ...domCases(),
  };
  for (const [name, run] of Object.entries(cases)) {
    record.cases.push([name, outcome(run)]);
  }
  return JSON.stringify(record);
})();
