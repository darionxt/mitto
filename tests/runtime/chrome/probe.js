// What the Runtime's tests compare with Chrome: the own properties of the
// interfaces that the Runtime implements, with their attributes, names and
// lengths, in order; what a call with the wrong receiver throws; and what some
// calls throw or do. It runs as it is in the Page program and, through
// record.html, in Chrome, and its value is the record as JSON.
//
// It records interfaces, descriptors, lengths and messages only, which say
// nothing of the machine that runs it.

(() => {
  const interfaces = ["DOMException", "Event", "EventTarget", "PopStateEvent"];

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
      toStringTag: object.prototype[Symbol.toStringTag],
      callWithoutNew: outcome(() => object()),
      constructWithoutArguments: outcome(() => new object()),
      wrongReceiver: wrongReceiver(object.prototype),
    };
  }

  const instances = {
    DOMException: () => new DOMException(),
    Event: () => new Event("x"),
    EventTarget: () => new EventTarget(),
    PopStateEvent: () => new PopStateEvent("x"),
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
  };
  for (const [name, run] of Object.entries(cases)) {
    record.cases.push([name, outcome(run)]);
  }
  return JSON.stringify(record);
})();
