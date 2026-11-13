// The probe: what a page's scripts can reach, which is its Script Fingerprint.
// It runs unchanged in real Chrome, for a Baseline, and in a Page, for a Parity
// Check. Run through an indirect eval, `(0, eval)(source)`, it gives a promise
// of the record as JSON's text, which the Page program prints and a Page's
// evaluate() returns as it is. docs/runtime.md, "The probe", says what it
// records and what it leaves out.
//
// It runs in four steps:
//
// 1. The walk. From globalThis, breadth first, every object it reaches once,
//    found first at its path, such as `navigator.userAgentData`. For each, its
//    own properties in order with their attributes, its [[Prototype]], and for
//    a function its name, length and source text. For an object that is
//    neither a function nor a prototype, what each getter on it and on its
//    prototype chain gives on it. For a function, what its own getters give.
// 2. The checks. What each function, getter and setter of a prototype, and of
//    an object whose [[Prototype]] is an interface's or a class's prototype,
//    throws when it is called on a new plain object, and what each constructor
//    does without `new`. Not the members of a namespace, such as console, Math
//    or CSS, or the static members of an interface, which belong to no kind of
//    object, and which a check would call.
// 3. The values that the Persona and the Machine decide beyond what the getters
//    give: the high entropy values, matchMedia() for each media feature that
//    Blink names, Intl's resolved options, the time zone through Date over a
//    year, the permission states and the permissions policy's lists. A worker
//    has no matchMedia() and no permissions policy.
// 4. What the walk itself made the page report, through a ReportingObserver.
//
// It runs in a window, top-level or a frame's, and in a dedicated worker, and
// the walk stays in its own realm, so that a frame's walk does not enter its
// parent's window.
//
// It calls no other function, so nothing that asks a person, opens something,
// or follows the machine beyond the values above, such as fonts, canvas, WebGL,
// audio or media capabilities, which M9 and M10 bring. It never waits for a
// promise that it finds, and gives each call that it waits for a time limit.
// It uses no regular expression, whose matches would change RegExp's legacy
// statics, which the walk reads.
//
// The record:
//
//   format    The version of this format.
//   objects   One entry for each object, in the order found:
//     path      Where the walk found it first, such as `Navigator.prototype`.
//     kind      "function", "prototype" (reached as a function's `prototype`
//               or an object's [[Prototype]], or whose own `constructor` has it
//               as its `prototype`) or "instance".
//     tag       What Object.prototype.toString gives for it, without
//               "[object " and "]".
//     proto     Its [[Prototype]], a value as below.
//     function  For a function: its own `name` and `length`, and its source
//               text when that is not `function NAME() { [native code] }`.
//     own       Its own properties in order: [key, flags, value] for a data
//               property, whose flags say w(ritable), e(numerable) and
//               c(onfigurable), with "-" for each that it is not,
//               [key, flags, getter, setter] for an accessor, with e and c, and
//               [key, ["throws", message]] for one whose descriptor throws. A
//               symbol's key is "@@" and its description, without "Symbol.".
//     ownKeys   Only there, as ["throws", message], when listing its own keys
//               throws.
//     get       For an instance or a function: [key, value] for each getter
//               that it reads, ["skipped"] for one in SKIPPED_GETTERS.
//     calls     For a prototype or an instance: [name, outcome] for each own
//               function, getter ("get NAME") and setter ("set NAME") called on
//               a new plain object, unless it threw `TypeError: Illegal
//               invocation`, as almost all do.
//     new       For a constructor, a function whose own `prototype` is not
//               writable: the outcome of calling it without `new`, or
//               ["skipped"] for one in SKIPPED_CONSTRUCTORS.
//   capped    Only there, and true, when it reached MAX_OBJECTS objects, after
//             which it walked no new one.
//   values    The values of step 3, each {"value": ...}, {"throws": message},
//             {"rejects": message} or {"timeout": true}, and {"value": ...,
//             "promise": false} for a call that should give a promise and gave
//             something else.
//   reports   [type, id] for each report that the page queued while the probe
//             ran, or {"throws": message} when the page has no
//             ReportingObserver.
//
// A value is JSON's own for a string, a finite number, true, false and null,
// and otherwise an array: ["undefined"], ["number", "NaN"], ["bigint", "1"],
// ["symbol", "Symbol(x)"], ["object", path], ["function", name, length]
// (with the source text when it is not the usual) for a function whose own
// properties are only its usual `length` and `name`, and ["function", length]
// for one among the own properties whose name is the one that its key gives
// it, such as "item", "get length" or "[Symbol.iterator]". ["node", nodeName]
// for a node of the DOM other than the document, which the walk does not
// enter, also one of another document, ["foreign", tag] for an object of
// another realm, such as the window of a frame's parent of the same origin,
// which the walk does not enter either, ["throws", message] for a getter that
// throws, and ["capped"]. An outcome is ["throws", message], ["returns",
// typeof], or for a promise ["fulfils"], ["rejects", message] or ["pending"],
// once the microtasks queued before have run.

(async () => {
  "use strict";

  // Taken before anything else runs, so that what the walk reads cannot change
  // what the probe itself calls.
  const getOwnPropertyDescriptor = Object.getOwnPropertyDescriptor;
  const getPrototypeOf = Object.getPrototypeOf;
  const objectIs = Object.is;
  const isArray = Array.isArray;
  const apply = Reflect.apply;
  const ownKeys = Reflect.ownKeys;
  const functionToString = Function.prototype.toString;
  const objectToString = Object.prototype.toString;
  const isPrototypeOf = Object.prototype.isPrototypeOf;
  const errorToString = Error.prototype.toString;
  const then = Promise.prototype.then;
  const ObjectPrototype = Object.prototype;
  const FunctionPrototype = Function.prototype;
  const PromisePrototype = Promise.prototype;
  const toStringTag = Symbol.toStringTag;
  const SafeMap = Map;
  const mapGet = Map.prototype.get;
  const mapSet = Map.prototype.set;
  const mapHas = Map.prototype.has;
  const stringify = JSON.stringify;

  const FORMAT = 1;
  const MAX_OBJECTS = 20000;
  const MAX_STRING = 4096;
  const TIME_LIMIT_MS = 5000;
  const MICROTASK_TURNS = 16;
  const USUAL_CHECK = "TypeError: Illegal invocation";

  // Getters that the walk does not call, by the name of the constructor whose
  // prototype holds each, or of the instance that holds it, with why. #207
  // read every other getter of Chrome 155, 156 and 157 and found none that
  // threw, took more than a millisecond, waited or changed what a page sees.
  const SKIPPED_GETTERS = new SafeMap([
    // Reading it queues a deprecation report, which a page's ReportingObserver
    // can take, in Chrome 156 and 157 (#207).
    ["Window.fence", true],
    // The same, in Chrome 156 and 157 (#207).
    ["Navigator.webkitPersistentStorage", true],
    // The same, in Chrome 155 and 156 without the variations seed (#207).
    ["Navigator.deprecatedRunAdAuctionEnforcesKAnonymity", true],
  ]);

  // Constructors that the checks do not call without `new`, by path, with why.
  const SKIPPED_CONSTRUCTORS = new SafeMap([
    // V8 makes a break iterator without `new` too, and Chrome then queues a
    // deprecation report, IntlV8BreakIterator, which a page's ReportingObserver
    // can take.
    ["Intl.v8BreakIterator", true],
  ]);

  // The hints of navigator.userAgentData.getHighEntropyValues(), in Chrome's
  // order of UserAgentDataValues.
  const HIGH_ENTROPY_HINTS = [
    "architecture",
    "bitness",
    "brands",
    "formFactors",
    "fullVersionList",
    "mobile",
    "model",
    "platform",
    "platformVersion",
    "uaFullVersion",
    "wow64",
  ];

  // Every name in Blink's media_feature_names.json5 of Chrome 157, in its
  // order. matchMedia() shows which of them a page's media queries take.
  const MEDIA_FEATURES = [
    "any-hover", "any-pointer", "block-size", "color", "color-index", "color-gamut", "forced-colors", "grid",
    "monochrome", "height", "hover", "width", "orientation", "aspect-ratio", "device-aspect-ratio", "dynamic-range",
    "video-dynamic-range", "-webkit-device-pixel-ratio", "device-height", "device-width", "display-mode",
    "display-state", "fallback", "inline-size", "max-color", "max-color-index", "max-aspect-ratio", "max-block-size",
    "min-block-size", "max-device-aspect-ratio", "-webkit-max-device-pixel-ratio", "max-device-height",
    "max-device-width", "max-height", "max-inline-size", "max-monochrome", "max-width", "max-resolution",
    "min-color", "min-color-index", "min-aspect-ratio", "min-device-aspect-ratio", "-webkit-min-device-pixel-ratio",
    "min-device-height", "min-device-width", "min-height", "min-inline-size", "min-monochrome", "min-width",
    "min-resolution", "navigation-controls", "origin-trial-test", "pointer", "prefers-color-scheme",
    "prefers-contrast", "prefers-reduced-motion", "prefers-reduced-data", "prefers-reduced-transparency",
    "resolution", "-webkit-transform-3d", "scan", "overflow-inline", "overflow-block", "update", "device-posture",
    "horizontal-viewport-segments", "vertical-viewport-segments", "snapped", "stuck", "inverted-colors", "scripting",
    "resizable", "scrollable", "scrolled",
  ];

  // The keywords that FeatureWithValidIdent() in Blink's media_query_exp.cc
  // takes for each media feature, in its order.
  const MEDIA_KEYWORDS = [
    ["display-mode", ["fullscreen", "unframed", "standalone", "minimal-ui", "window-controls-overlay", "browser",
      "tabbed", "picture-in-picture"]],
    ["display-state", ["fullscreen", "normal", "minimized", "maximized"]],
    ["resizable", ["true", "false"]],
    ["orientation", ["portrait", "landscape"]],
    ["pointer", ["none", "coarse", "fine"]],
    ["any-pointer", ["none", "coarse", "fine"]],
    ["hover", ["none", "hover"]],
    ["any-hover", ["none", "hover"]],
    ["scan", ["interlace", "progressive"]],
    ["color-gamut", ["srgb", "p3", "rec2020"]],
    ["inverted-colors", ["inverted", "none"]],
    ["prefers-color-scheme", ["dark", "light"]],
    ["prefers-contrast", ["no-preference", "more", "less", "custom"]],
    ["prefers-reduced-motion", ["no-preference", "reduce"]],
    ["dynamic-range", ["standard", "high"]],
    ["video-dynamic-range", ["standard", "high"]],
    ["prefers-reduced-data", ["no-preference", "reduce"]],
    ["prefers-reduced-transparency", ["no-preference", "reduce"]],
    ["forced-colors", ["none", "active"]],
    ["navigation-controls", ["none", "back-button"]],
    ["device-posture", ["continuous", "folded"]],
    ["overflow-inline", ["none", "scroll"]],
    ["overflow-block", ["none", "scroll", "paged"]],
    ["update", ["none", "fast", "slow"]],
    ["scripting", ["enabled", "initial-only", "none"]],
  ];

  // The media features that take an integer, and the largest that the probe
  // tries for each.
  const MEDIA_INTEGERS = [
    ["color", 16],
    ["color-index", 16],
    ["monochrome", 16],
    ["grid", 1],
    ["horizontal-viewport-segments", 4],
    ["vertical-viewport-segments", 4],
  ];

  // The names that navigator.permissions.query() is asked about, those of
  // Blink's PermissionName that a page can ask about, in order.
  const PERMISSIONS = [
    "accelerometer", "background-fetch", "background-sync", "camera", "captured-surface-control", "clipboard-read",
    "clipboard-write", "display-capture", "fullscreen", "geolocation", "gyroscope", "idle-detection",
    "keyboard-lock", "local-fonts", "local-network-access", "magnetometer", "microphone", "midi", "notifications",
    "payment-handler", "persistent-storage", "pointer-lock", "screen-wake-lock", "speaker-selection",
    "storage-access", "system-wake-lock", "top-level-storage-access", "web-app-installation", "window-management",
  ];

  // The instants that the time zone is read at: noon UTC on the first of each
  // month of 2026, and two days in it for Date's strings.
  const YEAR = 2026;
  const WINTER_DAY = Date.UTC(YEAR, 0, 15, 12);
  const SUMMER_DAY = Date.UTC(YEAR, 6, 15, 12);

  const setTimer = typeof setTimeout === "function" ? setTimeout : null;
  const clearTimer = typeof clearTimeout === "function" ? clearTimeout : null;

  // Arrays of the probe's own, filled without Array.prototype.push.
  const append = (list, item) => {
    list[list.length] = item;
  };

  const describeError = (error) => {
    try {
      if (error !== null && (typeof error === "object" || typeof error === "function")) {
        return apply(errorToString, error, []);
      }
      return `${error}`;
    } catch {
      return "unprintable";
    }
  };

  const isPromise = (value) => {
    try {
      return value !== null && typeof value === "object" && apply(isPrototypeOf, PromisePrototype, [value]);
    } catch {
      return false;
    }
  };

  // Keeps a promise that the page gives from being reported as rejected with
  // no handler, which a page could tell.
  const ignore = () => {};
  const handle = (promise) => {
    try {
      apply(then, promise, [ignore, ignore]);
    } catch {}
  };

  const tagOf = (object) => {
    try {
      const tag = apply(objectToString, object, []);
      return tag.slice(8, tag.length - 1);
    } catch (error) {
      return ["throws", describeError(error)];
    }
  };

  const isIdentifierStart = (code) =>
    (code >= 65 && code <= 90) || (code >= 97 && code <= 122) || code === 36 || code === 95;
  const isIdentifier = (key) => {
    if (key.length === 0 || !isIdentifierStart(key.charCodeAt(0))) {
      return false;
    }
    for (let index = 1; index < key.length; index++) {
      const code = key.charCodeAt(index);
      if (!isIdentifierStart(code) && !(code >= 48 && code <= 57)) {
        return false;
      }
    }
    return true;
  };

  const symbolName = (symbol) => {
    const description = symbol.description;
    if (typeof description !== "string") {
      return "@@";
    }
    return description.startsWith("Symbol.") ? `@@${description.slice(7)}` : `@@${description}`;
  };
  const keyName = (key) => (typeof key === "symbol" ? symbolName(key) : key);

  const ROOT = "globalThis";
  const childPath = (path, key) => {
    if (typeof key === "symbol") {
      return `${path}[${symbolName(key)}]`;
    }
    if (isIdentifier(key)) {
      return path === ROOT ? key : `${path}.${key}`;
    }
    return `${path}[${stringify(key)}]`;
  };

  const flagsOf = (descriptor) =>
    ("value" in descriptor ? (descriptor.writable ? "w" : "-") : "") +
    (descriptor.enumerable ? "e" : "-") +
    (descriptor.configurable ? "c" : "-");

  const sourceIfUnusual = (fn, name) => {
    let text;
    try {
      text = apply(functionToString, fn, []);
    } catch (error) {
      return ["throws", describeError(error)];
    }
    return text === `function ${name}() { [native code] }` ? null : text.slice(0, MAX_STRING);
  };

  // The name that a function gets from the key it is defined under: the key
  // for a method, "[Symbol.iterator]" for @@iterator, and with `prefix`, "get "
  // or "set ", for an accessor's.
  const nameFromKey = (key, prefix) => {
    if (typeof key !== "symbol") {
      return `${prefix}${key}`;
    }
    const description = key.description;
    return typeof description === "string" ? `${prefix}[${description}]` : prefix.slice(0, -1);
  };

  // A function whose own properties are only `length` and `name`, with their
  // usual attributes, and whose [[Prototype]] is Function.prototype, as almost
  // every method and accessor is, is written where it is found, without its
  // name when that is `expectedName`.
  const plainFunction = (fn, expectedName) => {
    try {
      if (getPrototypeOf(fn) !== FunctionPrototype) {
        return null;
      }
      const keys = ownKeys(fn);
      if (keys.length !== 2 || keys[0] !== "length" || keys[1] !== "name") {
        return null;
      }
      const length = getOwnPropertyDescriptor(fn, "length");
      const name = getOwnPropertyDescriptor(fn, "name");
      for (const descriptor of [length, name]) {
        if (!("value" in descriptor) || descriptor.writable || descriptor.enumerable || !descriptor.configurable) {
          return null;
        }
      }
      const encoded =
        name.value === expectedName ? ["function", length.value] : ["function", name.value, length.value];
      const source = sourceIfUnusual(fn, name.value);
      if (source !== null) {
        append(encoded, source);
      }
      return encoded;
    } catch {
      return null;
    }
  };

  // An object whose own `constructor` has it as its `prototype`.
  const hasOwnConstructor = (object) => {
    try {
      const descriptor = getOwnPropertyDescriptor(object, "constructor");
      if (!descriptor || !("value" in descriptor) || typeof descriptor.value !== "function") {
        return false;
      }
      const prototype = getOwnPropertyDescriptor(descriptor.value, "prototype");
      return !!prototype && prototype.value === object;
    } catch {
      return false;
    }
  };

  // A function whose own `prototype` is not writable: the interface objects
  // and the classes and constructors of the language.
  const isConstructor = (fn) => {
    try {
      const descriptor = getOwnPropertyDescriptor(fn, "prototype");
      return !!descriptor && "value" in descriptor && !descriptor.writable;
    } catch {
      return false;
    }
  };

  // A window has a document, and a worker has none.
  let theDocument = null;
  try {
    theDocument = typeof document === "object" ? document : null;
  } catch {}
  const inAWindow = theDocument !== null;
  let nodeName = null;
  let NodePrototype = null;
  try {
    if (typeof Node === "function") {
      NodePrototype = Node.prototype;
      nodeName = getOwnPropertyDescriptor(NodePrototype, "nodeName").get;
    }
  } catch {}

  const reports = [];
  let observer = null;
  try {
    observer = new ReportingObserver((list) => {
      for (const report of list) {
        append(reports, report);
      }
    }, { buffered: true });
    observer.observe();
  } catch (error) {
    observer = { error: describeError(error) };
  }

  // Step 1: the walk.

  // Whether an object belongs to another realm, such as the window of a
  // frame's parent and all that it holds: the last object on its prototype
  // chain is another realm's Object.prototype. An object with no prototype is
  // taken as this realm's, and so is a window of another origin, which gives
  // none, so that the walk reads the few members that it shows.
  const ofAnotherRealm = (object) => {
    let last = object;
    try {
      for (let next = getPrototypeOf(object); next !== null; next = getPrototypeOf(next)) {
        last = next;
      }
    } catch {
      return false;
    }
    return last !== ObjectPrototype && last !== object;
  };

  const found = new SafeMap();
  const objects = [];
  const queue = [];
  let head = 0;
  let capped = false;

  const reach = (object, path, asPrototype) => {
    const known = apply(mapGet, found, [object]);
    if (known !== undefined) {
      return known.path;
    }
    if (objects.length >= MAX_OBJECTS) {
      capped = true;
      return null;
    }
    let kind = "instance";
    if (typeof object === "function") {
      kind = hasOwnConstructor(object) ? "prototype" : "function";
    } else if (asPrototype || hasOwnConstructor(object)) {
      kind = "prototype";
    }
    const entry = { path, kind };
    apply(mapSet, found, [object, entry]);
    append(objects, entry);
    append(queue, object);
    return path;
  };

  const encode = (value, path, asPrototype, expectedName) => {
    switch (typeof value) {
      case "undefined":
        // document.all is an object whose typeof is "undefined".
        if (value === undefined) {
          return ["undefined"];
        }
        break;
      case "boolean":
        return value;
      case "number":
        return Number.isFinite(value) && !objectIs(value, -0) ? value : ["number", objectIs(value, -0) ? "-0" : `${value}`];
      case "string":
        return value.length > MAX_STRING ? `${value.slice(0, MAX_STRING)}...(${value.length})` : value;
      case "bigint":
        return ["bigint", `${value}`];
      case "symbol":
        return ["symbol", value.toString()];
      default:
        break;
    }
    if (value === null) {
      return null;
    }
    if (apply(mapGet, found, [value]) === undefined && ofAnotherRealm(value)) {
      // A node of another document, such as a frame's element in its
      // parent, which this realm's getter reads, or the window of another
      // frame of the same origin, which the walk does not enter.
      if (nodeName !== null) {
        try {
          return ["node", apply(nodeName, value, [])];
        } catch {}
      }
      return ["foreign", tagOf(value)];
    }
    if (typeof value === "function") {
      const plain = plainFunction(value, expectedName);
      if (plain !== null) {
        return plain;
      }
    } else if (NodePrototype !== null && value !== theDocument && !asPrototype && !hasOwnConstructor(value)) {
      // A node, but not the prototype of one of the DOM's interfaces, such as
      // Element.prototype, which has Node.prototype on its chain too, and
      // whose nodeName getter throws, so it is walked as an object.
      let isNode = false;
      try {
        isNode = apply(isPrototypeOf, NodePrototype, [value]);
      } catch {}
      if (isNode) {
        try {
          return ["node", apply(nodeName, value, [])];
        } catch {}
      }
    }
    const reached = reach(value, path, asPrototype);
    return reached === null ? ["capped"] : ["object", reached];
  };

  // The name of the constructor whose prototype holds a getter, or of the
  // object itself, for SKIPPED_GETTERS.
  const holderName = (holder, depth, entry) => {
    try {
      const descriptor = getOwnPropertyDescriptor(holder, "constructor");
      if (descriptor && "value" in descriptor && typeof descriptor.value === "function") {
        return `${descriptor.value.name}`;
      }
    } catch {}
    return depth === 0 && typeof entry.tag === "string" ? entry.tag : `${tagOf(holder)}`;
  };

  // What each getter on the object, and with `chain` on its prototype chain up
  // to Object.prototype, gives on the object.
  const readGetters = (object, entry, chain) => {
    const seen = new SafeMap();
    const read = [];
    let holder = object;
    for (let depth = 0; holder !== null && holder !== undefined && holder !== ObjectPrototype; depth++) {
      let keys;
      try {
        keys = ownKeys(holder);
      } catch {
        break;
      }
      const name = holderName(holder, depth, entry);
      for (const key of keys) {
        if (apply(mapHas, seen, [key])) {
          continue;
        }
        apply(mapSet, seen, [key, true]);
        let descriptor;
        try {
          descriptor = getOwnPropertyDescriptor(holder, key);
        } catch {
          continue;
        }
        if (!descriptor || !descriptor.get) {
          continue;
        }
        if (apply(mapHas, SKIPPED_GETTERS, [`${name}.${keyName(key)}`])) {
          append(read, [keyName(key), ["skipped"]]);
          continue;
        }
        let value;
        try {
          value = apply(descriptor.get, object, []);
        } catch (error) {
          append(read, [keyName(key), ["throws", describeError(error)]]);
          continue;
        }
        if (isPromise(value)) {
          handle(value);
        }
        append(read, [keyName(key), encode(value, childPath(entry.path, key), false)]);
      }
      if (!chain) {
        break;
      }
      try {
        holder = getPrototypeOf(holder);
      } catch {
        break;
      }
    }
    if (read.length > 0) {
      entry.get = read;
    }
  };

  // [[Prototype]]s wait until the objects that the walk has reached through
  // properties are visited, so that an object's path is a property's where it
  // has one, such as `Function.prototype` rather than `Object.[[Prototype]]`.
  const prototypes = [];

  const visit = (object, entry) => {
    entry.tag = tagOf(object);
    try {
      const proto = getPrototypeOf(object);
      entry.proto = null;
      if (proto !== null) {
        append(prototypes, [entry, proto]);
      }
    } catch (error) {
      entry.proto = ["throws", describeError(error)];
    }
    if (typeof object === "function") {
      let name = null;
      let length = null;
      try {
        const nameDescriptor = getOwnPropertyDescriptor(object, "name");
        if (nameDescriptor && "value" in nameDescriptor) {
          name = nameDescriptor.value;
        }
        const lengthDescriptor = getOwnPropertyDescriptor(object, "length");
        if (lengthDescriptor && "value" in lengthDescriptor) {
          length = lengthDescriptor.value;
        }
      } catch {}
      entry.function = [encode(name, entry.path, false), encode(length, entry.path, false)];
      const source = sourceIfUnusual(object, name);
      if (source !== null) {
        append(entry.function, source);
      }
    }
    const own = [];
    let keys = [];
    try {
      keys = ownKeys(object);
    } catch (error) {
      entry.ownKeys = ["throws", describeError(error)];
    }
    for (const key of keys) {
      const name = keyName(key);
      let descriptor;
      try {
        descriptor = getOwnPropertyDescriptor(object, key);
      } catch (error) {
        append(own, [name, ["throws", describeError(error)]]);
        continue;
      }
      if (!descriptor) {
        continue;
      }
      const path = childPath(entry.path, key);
      if ("value" in descriptor) {
        const asPrototype = key === "prototype" && typeof object === "function";
        append(own, [name, flagsOf(descriptor), encode(descriptor.value, path, asPrototype, nameFromKey(key, ""))]);
      } else {
        append(own, [
          name,
          flagsOf(descriptor),
          descriptor.get ? encode(descriptor.get, `get ${path}`, false, nameFromKey(key, "get ")) : null,
          descriptor.set ? encode(descriptor.set, `set ${path}`, false, nameFromKey(key, "set ")) : null,
        ]);
      }
    }
    entry.own = own;
    if (entry.kind === "instance") {
      readGetters(object, entry, true);
    } else if (entry.kind === "function") {
      readGetters(object, entry, false);
    }
  };

  reach(globalThis, ROOT, false);
  let prototypesHead = 0;
  for (;;) {
    if (head < queue.length) {
      visit(queue[head], objects[head]);
      head++;
    } else if (prototypesHead < prototypes.length) {
      const [entry, proto] = prototypes[prototypesHead];
      prototypesHead++;
      entry.proto = encode(proto, `${entry.path}.[[Prototype]]`, true);
    } else {
      break;
    }
  }

  // Step 2: the checks.

  const outcome = (call) => {
    let value;
    try {
      value = call();
    } catch (error) {
      return ["throws", describeError(error)];
    }
    if (isPromise(value)) {
      const result = ["pending"];
      try {
        apply(then, value, [
          () => {
            result[0] = "fulfils";
          },
          (error) => {
            result[0] = "rejects";
            result[1] = describeError(error);
          },
        ]);
      } catch (error) {
        result[0] = "throws";
        result[1] = describeError(error);
      }
      return result;
    }
    return ["returns", typeof value];
  };

  const isUsual = (result) => result[0] === "throws" && result[1] === USUAL_CHECK;

  // A prototype whose members belong to a kind of object: a constructor's, or
  // one with a @@toStringTag of its own, as every interface's prototype has,
  // also one with no interface object, such as StyleMedia's. Not console's
  // [[Prototype]], an empty object, so that console's own methods, which write
  // whatever their receiver, are not called.
  const ofAKind = (object) => {
    const entry = apply(mapGet, found, [object]);
    if (entry === undefined || entry.kind !== "prototype") {
      return false;
    }
    if (entry.ofAKind === true || hasOwnConstructor(object)) {
      return true;
    }
    try {
      const tag = getOwnPropertyDescriptor(object, toStringTag);
      return !!tag && typeof tag.value === "string";
    } catch {
      return false;
    }
  };

  for (let index = 0; index < queue.length; index++) {
    const object = queue[index];
    const entry = objects[index];
    if (entry.kind === "function") {
      if (isConstructor(object)) {
        entry.new = apply(mapHas, SKIPPED_CONSTRUCTORS, [entry.path]) ? ["skipped"] : outcome(() => object());
        const prototype = getOwnPropertyDescriptor(object, "prototype").value;
        const prototypeEntry = apply(mapGet, found, [prototype]);
        if (prototypeEntry !== undefined) {
          prototypeEntry.ofAKind = true;
        }
      }
    }
  }

  for (let index = 0; index < queue.length; index++) {
    const object = queue[index];
    const entry = objects[index];
    let checked = entry.kind === "prototype" && ofAKind(object);
    if (entry.kind === "instance") {
      try {
        const proto = getPrototypeOf(object);
        checked = proto !== null && proto !== ObjectPrototype && ofAKind(proto);
      } catch {}
    }
    if (!checked) {
      continue;
    }
    const calls = [];
    let keys = [];
    try {
      keys = ownKeys(object);
    } catch {}
    for (const key of keys) {
      let descriptor;
      try {
        descriptor = getOwnPropertyDescriptor(object, key);
      } catch {
        continue;
      }
      if (!descriptor) {
        continue;
      }
      const name = keyName(key);
      if ("value" in descriptor) {
        const fn = descriptor.value;
        if (typeof fn !== "function" || key === "constructor" || isConstructor(fn)) {
          continue;
        }
        const result = outcome(() => apply(fn, {}, []));
        if (!isUsual(result)) {
          append(calls, [name, result]);
        }
        continue;
      }
      if (descriptor.get) {
        const result = outcome(() => apply(descriptor.get, {}, []));
        if (!isUsual(result)) {
          append(calls, [`get ${name}`, result]);
        }
      }
      if (descriptor.set) {
        const result = outcome(() => apply(descriptor.set, {}, [undefined]));
        if (!isUsual(result)) {
          append(calls, [`set ${name}`, result]);
        }
      }
    }
    if (calls.length > 0) {
      entry.calls = calls;
    }
  }
  for (const entry of objects) {
    delete entry.ofAKind;
  }

  // The promises of the checks settle in the microtasks that their handlers
  // queued, which run before these turns end.
  for (let turn = 0; turn < MICROTASK_TURNS; turn++) {
    await undefined;
  }

  // Step 3: the values that the Persona and the Machine decide.

  const plain = (value, depth) => {
    if (value === undefined) {
      return ["undefined"];
    }
    if (value === null || typeof value === "boolean") {
      return value;
    }
    if (typeof value === "number") {
      return Number.isFinite(value) && !objectIs(value, -0) ? value : ["number", objectIs(value, -0) ? "-0" : `${value}`];
    }
    if (typeof value === "string") {
      return value.length > MAX_STRING ? `${value.slice(0, MAX_STRING)}...(${value.length})` : value;
    }
    if (typeof value === "bigint" || typeof value === "symbol") {
      return [typeof value, value.toString()];
    }
    if (typeof value === "function") {
      return ["function", `${value.name}`];
    }
    if (depth > 8) {
      return ["deep"];
    }
    if (isArray(value)) {
      const items = [];
      for (let index = 0; index < value.length; index++) {
        append(items, plain(value[index], depth + 1));
      }
      return items;
    }
    const result = {};
    for (const key of ownKeys(value)) {
      const descriptor = getOwnPropertyDescriptor(value, key);
      if (typeof key === "string" && descriptor.enumerable && "value" in descriptor) {
        result[key] = plain(descriptor.value, depth + 1);
      }
    }
    return result;
  };

  const check = (compute) => {
    try {
      return { value: plain(compute(), 0) };
    } catch (error) {
      return { throws: describeError(error) };
    }
  };

  // What a promise that a call gives settles with, within the time limit, and
  // what `pick` takes from its value.
  const settle = async (call, pick = (value) => value) => {
    let promise;
    try {
      promise = call();
    } catch (error) {
      return { throws: describeError(error) };
    }
    if (!isPromise(promise)) {
      return { value: plain(promise, 0), promise: false };
    }
    let timer = null;
    const result = await new Promise((resolve) => {
      apply(then, promise, [
        (value) => {
          try {
            resolve({ value: plain(pick(value), 0) });
          } catch (error) {
            resolve({ throws: describeError(error) });
          }
        },
        (error) => resolve({ rejects: describeError(error) }),
      ]);
      if (setTimer !== null) {
        timer = apply(setTimer, globalThis, [() => resolve({ timeout: true }), TIME_LIMIT_MS]);
      }
    });
    if (timer !== null && clearTimer !== null) {
      apply(clearTimer, globalThis, [timer]);
    }
    return result;
  };

  const matches = (query) => matchMedia(query).matches;
  const values = {};

  values["navigator.userAgentData.getHighEntropyValues()"] = await settle(() =>
    navigator.userAgentData.getHighEntropyValues(HIGH_ENTROPY_HINTS),
  );

  // matchMedia() and the permissions policy are a window's: a worker has
  // neither.
  if (inAWindow) {
    values["matchMedia(): the media text of each feature"] = check(() => {
      const result = {};
      for (const feature of MEDIA_FEATURES) {
        result[feature] = matchMedia(`(${feature})`).media;
      }
      return result;
    });
    values["matchMedia(): the keywords that match"] = check(() => {
      const result = {};
      for (const [feature, keywords] of MEDIA_KEYWORDS) {
        const matching = [];
        for (const keyword of keywords) {
          if (matches(`(${feature}: ${keyword})`)) {
            append(matching, keyword);
          }
        }
        result[feature] = matching;
      }
      return result;
    });
    values["matchMedia(): the integers that match"] = check(() => {
      const result = {};
      for (const [feature, largest] of MEDIA_INTEGERS) {
        const matching = [];
        for (let value = 0; value <= largest; value++) {
          if (matches(`(${feature}: ${value})`)) {
            append(matching, value);
          }
        }
        result[feature] = matching;
      }
      return result;
    });
    values["matchMedia(): what agrees with the window and the screen"] = check(() => ({
      "width = innerWidth": matches(`(width: ${innerWidth}px)`),
      "height = innerHeight": matches(`(height: ${innerHeight}px)`),
      "aspect-ratio = innerWidth / innerHeight": matches(`(aspect-ratio: ${innerWidth} / ${innerHeight})`),
      "device-width = screen.width": matches(`(device-width: ${screen.width}px)`),
      "device-height = screen.height": matches(`(device-height: ${screen.height}px)`),
      "device-aspect-ratio = screen.width / screen.height": matches(
        `(device-aspect-ratio: ${screen.width} / ${screen.height})`,
      ),
      "resolution = devicePixelRatio": matches(`(resolution: ${devicePixelRatio}dppx)`),
      "-webkit-device-pixel-ratio = devicePixelRatio": matches(`(-webkit-device-pixel-ratio: ${devicePixelRatio})`),
    }));
  }

  const resolved = (make) => check(() => make().resolvedOptions());
  values["new Intl.DateTimeFormat().resolvedOptions()"] = resolved(() => new Intl.DateTimeFormat());
  values["new Intl.DateTimeFormat(undefined, {hour: \"numeric\"}).resolvedOptions()"] = resolved(
    () => new Intl.DateTimeFormat(undefined, { hour: "numeric" }),
  );
  values["new Intl.NumberFormat().resolvedOptions()"] = resolved(() => new Intl.NumberFormat());
  values["new Intl.Collator().resolvedOptions()"] = resolved(() => new Intl.Collator());
  values["new Intl.PluralRules().resolvedOptions()"] = resolved(() => new Intl.PluralRules());
  values["new Intl.RelativeTimeFormat().resolvedOptions()"] = resolved(() => new Intl.RelativeTimeFormat());
  values["new Intl.ListFormat().resolvedOptions()"] = resolved(() => new Intl.ListFormat());
  values["new Intl.Segmenter().resolvedOptions()"] = resolved(() => new Intl.Segmenter());
  values["new Intl.DisplayNames(undefined, {type: \"region\"}).resolvedOptions()"] = resolved(
    () => new Intl.DisplayNames(undefined, { type: "region" }),
  );
  values["new Intl.DurationFormat().resolvedOptions()"] = resolved(() => new Intl.DurationFormat());
  values["Intl.supportedValuesOf(): how many of each"] = check(() => {
    const result = {};
    for (const key of ["calendar", "collation", "currency", "numberingSystem", "timeZone", "unit"]) {
      result[key] = Intl.supportedValuesOf(key).length;
    }
    return result;
  });

  values["Date: getTimezoneOffset() at noon UTC on the first of each month"] = check(() => {
    const offsets = [];
    for (let month = 0; month < 12; month++) {
      append(offsets, new Date(Date.UTC(YEAR, month, 1, 12)).getTimezoneOffset());
    }
    return offsets;
  });
  values["Date: the strings of a winter day and a summer day"] = check(() => {
    const result = {};
    for (const [name, time] of [["winter", WINTER_DAY], ["summer", SUMMER_DAY]]) {
      const date = new Date(time);
      result[name] = {
        toString: date.toString(),
        toTimeString: date.toTimeString(),
        toLocaleString: date.toLocaleString(),
        toLocaleDateString: date.toLocaleDateString(),
        toLocaleTimeString: date.toLocaleTimeString(),
        "timeZoneName long": new Intl.DateTimeFormat(undefined, { timeZoneName: "long" }).format(date),
      };
    }
    return result;
  });
  values["Number: toLocaleString() of 1234567.891"] = check(() => (1234567.891).toLocaleString());

  let permissions = null;
  try {
    permissions = navigator.permissions;
  } catch (error) {
    values["navigator.permissions.query(): the state of each"] = { throws: describeError(error) };
  }
  if (permissions !== null) {
    const states = {};
    for (const name of PERMISSIONS) {
      states[name] = await settle(
        () => permissions.query({ name }),
        (status) => status.state,
      );
    }
    values["navigator.permissions.query(): the state of each"] = { value: states };
  }
  if (inAWindow) {
    values["document.featurePolicy.allowedFeatures()"] = check(() => document.featurePolicy.allowedFeatures());
    values["document.featurePolicy.features()"] = check(() => document.featurePolicy.features());
  }

  // Step 4: what the page reported.

  let reported;
  if (observer.error !== undefined) {
    reported = { throws: observer.error };
  } else {
    try {
      for (const report of observer.takeRecords()) {
        append(reports, report);
      }
      observer.disconnect();
    } catch {}
    reported = [];
    for (const report of reports) {
      let id = null;
      try {
        id = report.body ? report.body.id : null;
      } catch {}
      append(reported, [report.type, id === undefined ? null : id]);
    }
  }

  const record = { format: FORMAT, objects };
  if (capped) {
    record.capped = true;
  }
  record.values = values;
  record.reports = reported;
  return stringify(record);
})();
