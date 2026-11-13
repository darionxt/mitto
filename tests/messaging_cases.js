// What structured clone, window.postMessage(), MessageChannel, MessageEvent
// and BroadcastChannel do in a page at an HTTP origin, once it has loaded,
// which tests/test_page_messaging.py runs in a Page that a Session opened and
// compares with what Chrome 157 gave for the same script, in
// tests/data/messaging_cases.json. Its value is a promise of the record.

(async () => {
  const out = {};
  const tick = (ms = 0) => new Promise((resolve) => setTimeout(resolve, ms));
  // Continues in a task that no timer nests, so that a timer set next is not
  // clamped for nesting.
  const hop = () => new Promise((resolve) => {
    const channel = new MessageChannel();
    channel.port1.onmessage = () => {
      channel.port1.close();
      resolve();
    };
    channel.port2.postMessage(0);
  });
  const outcome = (f) => {
    try {
      f();
      return "ok";
    } catch (e) {
      return `${e?.name}: ${e?.message}`;
    }
  };

  // A description of a value that JSON keeps, with what a clone changes.
  const show = (value, seen = new Map()) => {
    if (typeof value === "bigint") return `${value}n`;
    if (typeof value === "number") {
      return Object.is(value, -0) ? "-0" : Number.isFinite(value) ? value : String(value);
    }
    if (value === undefined) return "undefined";
    if (value === null || typeof value !== "object") return value;
    if (seen.has(value)) return `ref ${seen.get(value)}`;
    seen.set(value, seen.size);
    const tag = Object.prototype.toString.call(value);
    const proto = Object.getPrototypeOf(value);
    const kind = proto === null ? "null prototype" : proto.constructor?.name;
    const own = (object) =>
      Object.fromEntries(Object.keys(object).map((key) => [key, show(object[key], seen)]));
    if (value instanceof Date) return [kind, value.getTime()];
    if (value instanceof RegExp) return [kind, value.source, value.flags, value.lastIndex];
    if (value instanceof Map) return [kind, [...value].map(([k, v]) => [show(k, seen), show(v, seen)])];
    if (value instanceof Set) return [kind, [...value].map((v) => show(v, seen))];
    if (value instanceof ArrayBuffer) {
      return [kind, value.byteLength, value.resizable, value.maxByteLength, [...new Uint8Array(value)]];
    }
    if (ArrayBuffer.isView(value)) {
      return [kind, value.byteOffset, value.byteLength, show(value.buffer, seen)];
    }
    if (value instanceof DOMException) {
      return [kind, value.name, value.message, value.code, Object.keys(value), typeof value.stack];
    }
    if (value instanceof Error) {
      return [kind, value.name, value.message, "cause" in value ? show(value.cause, seen) : "no cause",
        Object.getOwnPropertyNames(value).filter((key) => key !== "stack").sort(), typeof value.stack];
    }
    if (typeof DOMRectReadOnly !== "undefined" && value instanceof DOMRectReadOnly) {
      return [kind, value.x, value.y, value.width, value.height].map((v) => show(v, seen));
    }
    if (value instanceof Number || value instanceof String || value instanceof Boolean ||
        value instanceof BigInt) {
      return [kind, show(value.valueOf(), seen)];
    }
    if (Array.isArray(value)) return [kind, value.length, own(value)];
    if (tag !== "[object Object]") return [kind, tag];
    return [kind, own(value)];
  };

  const clone = (value, options) => {
    try {
      return show(structuredClone(value, options));
    } catch (e) {
      return `${e?.name}: ${e?.message}`;
    }
  };

  // Values that a clone copies.
  const shared = new ArrayBuffer(8);
  const cyclic = { name: "c" };
  cyclic.self = cyclic;
  cyclic.list = [cyclic, cyclic];
  const regexp = /a+b/gimsy;
  regexp.lastIndex = 3;
  const sparse = [1, , 3];
  sparse.extra = "x";
  const error = new TypeError("t", { cause: { c: 1 } });
  error.extra = 1;
  const renamed = new Error("r");
  renamed.name = "Foo";
  out.values = {
    primitives: clone([undefined, null, true, 1.5, -0, NaN, -Infinity, 12345678901234567890n, "hé😀"]),
    loneSurrogate: (() => {
      const s = structuredClone("a\ud800b");
      return [s.length, s.charCodeAt(1)];
    })(),
    boxed: clone([new Number(3), new String("ab"), new Boolean(false), Object(5n)]),
    date: clone(new Date(1700000000123)),
    invalidDate: clone(new Date(NaN)),
    regexp: clone(regexp),
    map: clone(new Map([[1, { a: 1 }], ["k", [2]]])),
    set: clone(new Set([1, "a", { b: 2 }])),
    sparse: clone(sparse),
    nested: clone({ a: { b: { c: [1, { d: 2 }] } } }),
    nullPrototype: clone(Object.assign(Object.create(null), { x: 1 })),
    classInstance: clone(new (class Foo {
      constructor() {
        this.v = 1;
      }
      get g() {
        return 2;
      }
    })()),
    getter: clone({ get a() {
      return "got";
    } }),
    nonEnumerable: clone(Object.defineProperty({ k: 1 }, "hidden", { value: 1, enumerable: false })),
    symbolKey: clone({ [Symbol("s")]: 1, k: 2 }),
    cyclic: clone(cyclic),
    arrayBuffer: clone(new Uint8Array([1, 2, 3]).buffer),
    resizable: clone(new ArrayBuffer(4, { maxByteLength: 16 })),
    views: clone([new Uint8Array(shared, 1, 3), new Float64Array(shared), new DataView(shared, 2, 4),
      new BigInt64Array([1n]), new Uint8ClampedArray([300])]),
    float16: typeof Float16Array === "undefined" ? "none" : clone(new Float16Array([1.5])),
    errors: clone([new Error("m"), error, new RangeError("r"), new SyntaxError("s"), new ReferenceError("f"),
      new EvalError("e"), new URIError("u"), renamed, new AggregateError([1], "a"), new Error(5)]),
    domException: clone([new DOMException("msg", "NotFoundError"), new DOMException("m", "Custom"),
      new DOMException()]),
    domRect: clone([new DOMRect(1, 2, 3, 4), new DOMRectReadOnly(1, 2, NaN, -4)]),
    quotaExceeded: typeof QuotaExceededError === "undefined" ? "none" : (() => {
      const e = structuredClone(new QuotaExceededError("q", { quota: 5, requested: 10 }));
      const n = structuredClone(new QuotaExceededError("n"));
      return [e.constructor.name, e.name, e.message, e.code, e.quota, e.requested, n.quota, n.requested];
    })(),
    wasmModule: (() => {
      const module = new WebAssembly.Module(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]));
      const copy = structuredClone(module);
      return [Object.prototype.toString.call(copy), copy === module, copy instanceof WebAssembly.Module];
    })(),
  };

  // Objects of interfaces that Blink copies but that the Runtime has no class
  // for: only their interface is compared.
  const interfaceOf = (value) => {
    try {
      const copy = structuredClone(value);
      return [Object.prototype.toString.call(copy), copy === value];
    } catch (e) {
      return `${e?.name}: ${e?.message}`;
    }
  };
  out.otherInterfaces = {
    domPoint: interfaceOf(new DOMPoint(1, 2)),
    domPointReadOnly: interfaceOf(new DOMPointReadOnly(1, 2)),
    domMatrix: interfaceOf(new DOMMatrix()),
    domQuad: interfaceOf(new DOMQuad()),
    imageData: interfaceOf(new ImageData(1, 1)),
    blob: interfaceOf(new Blob(["a"])),
    file: interfaceOf(new File(["a"], "f")),
  };

  // Values that a clone refuses.
  const detached = new ArrayBuffer(1);
  structuredClone(detached, { transfer: [detached] });
  const memory = new WebAssembly.Memory({ initial: 1, maximum: 1, shared: true });
  out.refused = {
    arrow: clone(() => {}),
    named: clone(function named() {}),
    klass: clone(class K {}),
    symbol: clone(Symbol("x")),
    weakMap: clone(new WeakMap()),
    weakSet: clone(new WeakSet()),
    weakRef: clone(new WeakRef({})),
    promise: clone(Promise.resolve()),
    proxy: clone(new Proxy({}, {})),
    generator: clone((function* gen() {})()),
    inObject: clone({ a: { b: () => 1 } }),
    inMap: clone(new Map([[1, Symbol.iterator]])),
    document: clone(document),
    window: clone(window),
    element: clone(document.createElement("div")),
    text: clone(document.createTextNode("t")),
    event: clone(new Event("x")),
    abortSignal: clone(new AbortController().signal),
    headers: clone(new Headers()),
    response: clone(new Response("")),
    location: clone(location),
    navigator: clone(navigator),
    textEncoder: clone(new TextEncoder()),
    messagePort: clone(new MessageChannel().port1),
    messageChannel: clone(new MessageChannel()),
    broadcastChannel: (() => {
      const channel = new BroadcastChannel("refused");
      const result = clone(channel);
      channel.close();
      return result;
    })(),
    getterThrows: clone({ get a() {
      throw new RangeError("boom");
    } }),
    detached: clone(detached),
    sharedArrayBuffer: clone(memory.buffer),
    sharedInObject: clone({ a: memory.buffer }),
  };

  // Transfers.
  out.transfer = {};
  {
    const buffer = new Uint8Array([1, 2, 3]).buffer;
    const copy = structuredClone(buffer, { transfer: [buffer] });
    out.transfer.arrayBuffer = [buffer.byteLength, buffer.detached, copy === buffer, show(copy)];
  }
  {
    const view = new Uint8Array([1, 2, 3, 4]).subarray(1, 3);
    const copy = structuredClone(view, { transfer: [view.buffer] });
    out.transfer.view = [view.length, show(copy)];
  }
  {
    const buffer = new ArrayBuffer(4, { maxByteLength: 16 });
    const copy = structuredClone({ a: buffer, b: buffer }, { transfer: [buffer] });
    out.transfer.resizable = [buffer.detached, copy.a === copy.b, copy.a.resizable, copy.a.maxByteLength];
  }
  {
    const buffer = new ArrayBuffer(2);
    out.transfer.notInValue = [clone(1, { transfer: [buffer] }), buffer.detached];
  }
  {
    const a = new ArrayBuffer(1);
    const b = new ArrayBuffer(1);
    const log = [];
    out.transfer.errors = {
      nullValue: clone(1, { transfer: [null] }),
      number: clone(1, { transfer: [1] }),
      plainObject: clone(1, { transfer: [{}] }),
      duplicate: clone(1, { transfer: [a, b, a] }),
      detached: clone(1, { transfer: [b, detached] }),
      notIterable: clone(1, { transfer: {} }),
      string: clone(1, { transfer: "ab" }),
      options: clone(1, 5),
      nullOptions: clone(1, null),
      sharedArrayBuffer: clone(1, { transfer: [memory.buffer] }),
      wasmMemory: clone(1, { transfer: [new WebAssembly.Memory({ initial: 1 }).buffer] }),
      duplicatePort: (() => {
        const { port1 } = new MessageChannel();
        return clone(1, { transfer: [port1, port1] });
      })(),
      getterBeforeDetached: [clone({ get a() {
        log.push("got");
        return 1;
      } }, { transfer: [detached] }), log],
      detachedAfterBadValue: (() => {
        const c = new ArrayBuffer(1);
        return [clone({ a: () => {} }, { transfer: [c] }), c.detached];
      })(),
      detachedBeforeSharedError: (() => {
        const c = new ArrayBuffer(1);
        return [clone(memory.buffer, { transfer: [c] }), c.detached];
      })(),
      transferredTwice: (() => {
        const { port1 } = new MessageChannel();
        structuredClone(port1, { transfer: [port1] });
        return clone(1, { transfer: [port1] });
      })(),
    };
  }
  {
    const { port1, port2 } = new MessageChannel();
    const copy = structuredClone({ p: port1 }, { transfer: [port1] });
    const received = new Promise((resolve) => {
      port2.onmessage = (e) => resolve(e.data);
    });
    copy.p.postMessage("through the copy");
    out.transfer.port = [copy.p instanceof MessagePort, copy.p === port1, outcome(() => port1.postMessage(() => {})),
      await received];
  }

  // window.postMessage() to the window itself.
  const events = [];
  const describeEvent = (e) => [e.constructor.name, e.type, show(e.data), e.origin, e.source === window,
    e.lastEventId, e.ports.length, Object.isFrozen(e.ports), e.isTrusted, e.bubbles, e.cancelable, e.composed,
    e.data === e.data, e.ports === e.ports, e.userActivation === null ? null
      : [e.userActivation.constructor.name, e.userActivation.hasBeenActive, e.userActivation.isActive]];
  const onWindowMessage = (e) => events.push(e);
  window.addEventListener("message", onWindowMessage);
  const take = async () => {
    await tick(20);
    return events.splice(0).map(describeEvent);
  };
  window.postMessage({ a: [1] }, "*");
  out.windowPostMessage = { basic: await take() };
  const posts = {
    origin: [{ v: "origin" }, location.origin],
    slash: ["slash", "/"],
    otherHost: ["other host", "https://example.com"],
    otherPort: ["other port", "http://127.0.0.1:1"],
    trailingPath: ["path", `${location.origin}/some/path?q#f`],
  };
  for (const [key, [data, target]] of Object.entries(posts)) {
    const thrown = outcome(() => window.postMessage(data, target));
    out.windowPostMessage[key] = [thrown, await take()];
  }
  {
    const throws = {
      noArguments: () => window.postMessage(),
      invalidOrigin: () => window.postMessage(1, "foo"),
      emptyOrigin: () => window.postMessage(1, ""),
      dataUrlOrigin: () => window.postMessage(1, "data:,x"),
      cloneBeforeOrigin: () => window.postMessage(() => {}, "foo"),
      stringTransfer: () => window.postMessage(1, "*", "x"),
      delegateStar: () => window.postMessage(1, { targetOrigin: "*", delegate: "payment" }),
      delegateUnknown: () => window.postMessage(1, { delegate: "foo" }),
      delegateNoActivation: () => window.postMessage(1, { delegate: "payment" }),
    };
    out.windowPostMessage.errors = Object.fromEntries(
      Object.entries(throws).map(([key, f]) => [key, outcome(f)]));
    out.windowPostMessage.afterErrors = await take();
  }
  {
    const buffer = new ArrayBuffer(4);
    const thrown = outcome(() => window.postMessage(buffer, "foo", [buffer]));
    out.windowPostMessage.detachedBeforeOrigin = [thrown, buffer.detached];
  }
  {
    window.postMessage("options");
    window.postMessage("undefined options", undefined);
    window.postMessage("null options", null);
    window.postMessage("activation", { targetOrigin: "*", includeUserActivation: true });
    const buffer = new Uint8Array([7]).buffer;
    window.postMessage({ buffer }, { transfer: [buffer] });
    out.windowPostMessage.optionForms = [await take(), buffer.detached];
  }
  {
    const { port1, port2 } = new MessageChannel();
    window.postMessage("with a port", "*", [port2]);
    const [event] = events.length ? events : await new Promise((resolve) => setTimeout(() => resolve(events), 20));
    events.length = 0;
    const port = event.ports[0];
    const reply = new Promise((resolve) => {
      port1.onmessage = (e) => resolve(e.data);
    });
    port.postMessage("reply");
    out.windowPostMessage.port = [event.ports.length, port instanceof MessagePort, port === port2, await reply];
  }
  {
    await hop();
    const order = [];
    window.removeEventListener("message", onWindowMessage);
    const listener = (e) => order.push(`message ${e.data}`);
    window.addEventListener("message", listener);
    window.postMessage(1, "*");
    setTimeout(() => order.push("timeout"), 0);
    window.postMessage(2, "*");
    Promise.resolve().then(() => order.push("microtask"));
    await tick(30);
    window.removeEventListener("message", listener);
    window.addEventListener("message", onWindowMessage);
    out.windowPostMessage.order = order;
  }
  {
    let inHandler = null;
    window.onmessage = (e) => {
      inHandler = [e.data, window.event === e];
    };
    window.postMessage("handler", "*");
    await tick(20);
    window.onmessage = null;
    events.length = 0;
    out.windowPostMessage.handler = inHandler;
  }
  window.removeEventListener("message", onWindowMessage);

  // MessageChannel and MessagePort.
  out.messageChannel = {};
  {
    const channel = new MessageChannel();
    out.messageChannel.shape = [channel.port1 === channel.port1, channel.port1 !== channel.port2,
      Object.prototype.toString.call(channel.port1), channel.port1 instanceof EventTarget,
      Object.getOwnPropertyNames(MessagePort.prototype).sort(), Object.getOwnPropertyNames(MessageChannel.prototype).sort(),
      MessageChannel.length, outcome(() => MessageChannel()), outcome(() => new MessagePort()),
      channel.port1.onmessage, channel.port1.onmessageerror, "onclose" in channel.port1];
  }
  {
    const { port1, port2 } = new MessageChannel();
    const got = [];
    port2.addEventListener("message", (e) => got.push(e.data));
    port1.postMessage("before start");
    await tick(20);
    const beforeStart = got.slice();
    port2.start();
    port2.start();
    await tick(20);
    out.messageChannel.start = [beforeStart, got];
  }
  {
    const { port1, port2 } = new MessageChannel();
    let event = null;
    port2.onmessage = (e) => {
      event = e;
    };
    port1.postMessage({ x: 1 });
    await tick(20);
    out.messageChannel.event = [event.constructor.name, event.type, show(event.data), event.origin, event.source,
      event.lastEventId, event.ports.length, Object.isFrozen(event.ports), event.isTrusted, event.bubbles,
      event.cancelable, event.target === port2, event.currentTarget === port2, event.userActivation];
  }
  {
    const { port1, port2 } = new MessageChannel();
    let started = false;
    port2.onmessageerror = () => {};
    port2.addEventListener("message", () => {
      started = true;
    });
    port1.postMessage(1);
    await tick(20);
    out.messageChannel.onmessageerrorStarts = started;
  }
  {
    await hop();
    const { port1, port2 } = new MessageChannel();
    const order = [];
    port2.onmessage = (e) => {
      order.push(`message ${e.data}`);
      Promise.resolve().then(() => order.push(`microtask ${e.data}`));
    };
    port1.postMessage(1);
    port1.postMessage(2);
    setTimeout(() => order.push("timeout"), 0);
    port1.postMessage(3);
    await tick(30);
    out.messageChannel.order = order;
  }
  {
    await hop();
    const order = [];
    const start = new MessageChannel();
    start.port2.addEventListener("message", (e) => order.push(`start ${e.data}`));
    start.port1.postMessage(1);
    start.port1.postMessage(2);
    await tick(20);
    await hop();
    setTimeout(() => order.push("timeout A"), 0);
    start.port2.start();
    setTimeout(() => order.push("timeout B"), 0);
    start.port1.postMessage(3);
    const other = new MessageChannel();
    other.port2.onmessage = (e) => order.push(`other ${e.data}`);
    other.port1.postMessage(1);
    window.addEventListener("message", function listener(e) {
      order.push(`window ${e.data}`);
      if (e.data === 2) window.removeEventListener("message", listener);
    });
    window.postMessage(1, "*");
    other.port1.postMessage(2);
    window.postMessage(2, "*");
    await tick(30);
    out.messageChannel.orderAcrossQueues = order;
  }
  {
    const { port1, port2 } = new MessageChannel();
    const got = [];
    port2.onmessage = (e) => got.push(e.data);
    port1.postMessage("sent before close");
    port1.close();
    port1.close();
    await tick(20);
    out.messageChannel.closeSender = [got, outcome(() => port1.postMessage(() => {})),
      outcome(() => port2.postMessage(() => {}))];
  }
  {
    const { port1, port2 } = new MessageChannel();
    const got = [];
    port2.onmessage = (e) => got.push(e.data);
    port1.postMessage("never read");
    port2.close();
    await tick(20);
    out.messageChannel.closeReceiver = got;
  }
  {
    const { port1 } = new MessageChannel();
    out.messageChannel.errors = {
      clone: outcome(() => port1.postMessage(() => {})),
      sourcePort: outcome(() => port1.postMessage(1, [port1])),
      sourcePortInOptions: outcome(() => port1.postMessage(1, { transfer: [port1] })),
      noArguments: outcome(() => port1.postMessage()),
      number: outcome(() => port1.postMessage(1, 5)),
      string: outcome(() => port1.postMessage(1, "ab")),
      nullTransfer: outcome(() => port1.postMessage(1, null)),
      nullInList: outcome(() => port1.postMessage(1, [null])),
    };
  }
  {
    const first = new MessageChannel();
    const second = new MessageChannel();
    second.port2.postMessage("queued before the transfer");
    first.port1.postMessage("carrying a port", [second.port1]);
    const got = [];
    const received = new Promise((resolve) => {
      first.port2.onmessage = (e) => {
        const port = e.ports[0];
        port.onmessage = (inner) => {
          got.push(inner.data);
          if (got.length === 2) resolve();
        };
        second.port2.postMessage("sent after the transfer");
      };
    });
    await received;
    out.messageChannel.transferredPort = [got, outcome(() => second.port1.postMessage(() => {}))];
  }
  {
    const { port1, port2 } = new MessageChannel();
    const got = [];
    port2.onmessage = (e) => got.push([e.data, e.userActivation === null ? null
      : [e.userActivation.constructor.name, e.userActivation.hasBeenActive, e.userActivation.isActive]]);
    port1.postMessage(1, { includeUserActivation: true, transfer: [] });
    port1.postMessage(2, []);
    await tick(20);
    out.messageChannel.userActivation = got;
  }

  // MessageEvent.
  {
    const { port1 } = new MessageChannel();
    const data = { d: 1 };
    const plain = new MessageEvent("message");
    const full = new MessageEvent("m", { data, origin: "o", lastEventId: "id", source: window, ports: [port1],
      bubbles: true, cancelable: true });
    const portSource = new MessageEvent("m", { source: port1 });
    out.messageEvent = {
      plain: [show(plain.data), plain.origin, plain.lastEventId, plain.source, plain.ports.length,
        Object.isFrozen(plain.ports), plain.userActivation, plain.bubbles, plain.isTrusted],
      full: [full.data === data, full.origin, full.lastEventId, full.source === window, full.ports.length,
        full.ports[0] === port1, full.ports === full.ports, full.bubbles, full.cancelable],
      portSource: portSource.source === port1,
      undefinedData: show(new MessageEvent("m", { data: undefined }).data),
      errors: {
        noArguments: outcome(() => new MessageEvent()),
        call: outcome(() => MessageEvent("m")),
        eventTargetSource: outcome(() => new MessageEvent("m", { source: new EventTarget() })),
        documentSource: outcome(() => new MessageEvent("m", { source: document })),
        objectSource: outcome(() => new MessageEvent("m", { source: {} })),
        badPorts: outcome(() => new MessageEvent("m", { ports: [1] })),
        portsNotIterable: outcome(() => new MessageEvent("m", { ports: 1 })),
        badActivation: outcome(() => new MessageEvent("m", { userActivation: {} })),
      },
      shape: Object.getOwnPropertyNames(MessageEvent.prototype).sort(),
    };
    const e = new MessageEvent("x");
    e.initMessageEvent("y", true, true, 5, "o", "id", window, [port1]);
    out.messageEvent.init = [e.type, e.bubbles, e.cancelable, e.data, e.origin, e.lastEventId, e.source === window,
      e.ports.length, Object.isFrozen(e.ports)];
    e.initMessageEvent("z");
    out.messageEvent.initDefaults = [e.type, e.bubbles, e.data, e.origin, e.lastEventId, e.source, e.ports.length];
    out.messageEvent.initNoArguments = outcome(() => e.initMessageEvent());
    const activation = navigator.userActivation;
    out.messageEvent.activation = [new MessageEvent("m", { userActivation: activation }).userActivation === activation,
      Object.prototype.toString.call(activation), activation.hasBeenActive, activation.isActive];
    let during = null;
    const target = new EventTarget();
    target.addEventListener("t", (event) => {
      event.initMessageEvent("changed", false, false, "new data");
      during = [event.type, event.data];
    });
    target.dispatchEvent(new MessageEvent("t", { data: "old data" }));
    out.messageEvent.initDuringDispatch = during;
  }

  // BroadcastChannel within the document.
  out.broadcastChannel = {};
  {
    out.broadcastChannel.shape = [Object.getOwnPropertyNames(BroadcastChannel.prototype).sort(),
      outcome(() => new BroadcastChannel()), outcome(() => BroadcastChannel("x")),
      (() => {
        const a = new BroadcastChannel(5);
        const b = new BroadcastChannel(null);
        const result = [a.name, b.name, a.onmessage, a.onmessageerror];
        a.close();
        b.close();
        return result;
      })()];
  }
  {
    const got = [];
    const a = new BroadcastChannel("t");
    const b = new BroadcastChannel("t");
    const c = new BroadcastChannel("t");
    const other = new BroadcastChannel("other");
    for (const [name, channel] of Object.entries({ a, b, c, other })) {
      channel.onmessage = (e) => got.push([name, show(e.data), e.origin, e.source, e.lastEventId, e.ports.length,
        e.isTrusted, e.constructor.name, e.target === channel]);
    }
    a.postMessage({ x: [1] });
    const late = new BroadcastChannel("t");
    late.onmessage = (e) => got.push(["late", show(e.data)]);
    await tick(30);
    out.broadcastChannel.delivery = got.splice(0);
    b.close();
    c.postMessage("after b closed");
    await tick(30);
    out.broadcastChannel.afterClose = got.splice(0);
    b.onmessage = null;
    a.postMessage("to close");
    c.close();
    await tick(30);
    out.broadcastChannel.closedBeforeDelivery = got.splice(0);
    out.broadcastChannel.errors = {
      closed: outcome(() => b.postMessage(1)),
      closedClone: outcome(() => b.postMessage(() => {})),
      clone: outcome(() => a.postMessage(() => {})),
      noArguments: outcome(() => a.postMessage()),
      sharedArrayBuffer: outcome(() => a.postMessage(memory.buffer)),
    };
    for (const channel of [a, late, other]) channel.close();
  }
  {
    await hop();
    const order = [];
    const a = new BroadcastChannel("order");
    const b = new BroadcastChannel("order");
    b.onmessage = (e) => order.push(`message ${e.data}`);
    a.postMessage(1);
    setTimeout(() => order.push("timeout"), 0);
    a.postMessage(2);
    await tick(30);
    a.close();
    b.close();
    out.broadcastChannel.order = order;
  }
  {
    const a = new BroadcastChannel("module");
    const b = new BroadcastChannel("module");
    const got = [];
    b.onmessage = (e) => got.push(Object.prototype.toString.call(e.data));
    b.onmessageerror = () => got.push("messageerror");
    a.postMessage(new WebAssembly.Module(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0])));
    await tick(30);
    a.close();
    b.close();
    out.broadcastChannel.wasmModule = got;
  }
  return out;
})()
