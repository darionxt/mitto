// A throwaway walk for darionxt/mitto#207, never merged: everything a page can
// reach from globalThis, what its getters return, and some calls whose results
// may follow the machine. walk.html runs it through an indirect eval and posts
// its value, an object, to the server that served it.
//
// Each object it reaches is a node, found first at a path. A node lists its own
// properties in order with their attributes. A function whose own properties
// are only `length` and `name`, with the usual attributes and
// Function.prototype as its prototype, is written in place as
// ["F", name, length] instead of a node, with its source text when that is not
// `function NAME() { [native code] }`. A node that is neither a function nor a
// prototype gets `rd`: what each getter on it and its prototype chain returns
// for it. A function gets the values of its own getters. Nodes of the DOM
// other than the document are written as ["N", nodeName] and not walked.
(async () => {
  "use strict";
  const started = performance.now();
  const getOwnPropertyDescriptor = Object.getOwnPropertyDescriptor;
  const getPrototypeOf = Object.getPrototypeOf;
  const ownKeys = Reflect.ownKeys;
  const apply = Reflect.apply;
  const fnToString = Function.prototype.toString;
  const objToString = Object.prototype.toString;
  const isPrototypeOf = Object.prototype.isPrototypeOf;
  const FunctionPrototype = Function.prototype;
  const ObjectPrototype = Object.prototype;
  const PromisePrototype = Promise.prototype;
  const then = Promise.prototype.then;
  const now = () => performance.now();
  const NodePrototype = typeof Node === "function" ? Node.prototype : null;
  const nodeName = NodePrototype && getOwnPropertyDescriptor(NodePrototype, "nodeName").get;
  const theDocument = typeof document === "object" ? document : null;
  const round = (ms) => Math.round(ms * 100) / 100;

  // Getters the walk does not call: "Label.name", where Label is the name of
  // the constructor whose prototype holds the getter, or of the object itself.
  // The page's address names them in `skip`, separated by commas.
  const SKIP = new Set((new URLSearchParams(location.search).get("skip") || "").split(",").filter(Boolean));
  const MAX_NODES = 200000;
  const MAX_STRING = 2000;

  const reports = [];
  let observer = null;
  try {
    observer = new ReportingObserver(() => {}, { buffered: true });
    observer.observe();
  } catch (error) {
    reports.push(["observer", String(error)]);
  }
  const takeReports = (label) => {
    if (!observer) return;
    let list;
    try {
      list = observer.takeRecords();
    } catch {
      return;
    }
    for (const report of list) {
      const body = report.body || {};
      reports.push([label, report.type, body.id || null, body.message || null]);
    }
  };
  takeReports("before the walk");

  const keyName = (key) => (typeof key === "symbol" ? `@@${key.description}` : key);
  const tagOf = (object) => {
    try {
      return apply(objToString, object, []).slice(8, -1);
    } catch (error) {
      return `throws ${error}`;
    }
  };
  const flags = (d) =>
    ("value" in d ? (d.writable ? "w" : "-") : "") + (d.enumerable ? "e" : "-") + (d.configurable ? "c" : "-");

  const sourceDeviation = (f, name) => {
    let text;
    try {
      text = apply(fnToString, f, []);
    } catch (error) {
      return `throws ${error}`;
    }
    return text === `function ${name}() { [native code] }` ? null : text.slice(0, 400);
  };

  const plainFunction = (f) => {
    try {
      if (getPrototypeOf(f) !== FunctionPrototype) return null;
      const keys = ownKeys(f);
      if (keys.length !== 2 || keys[0] !== "length" || keys[1] !== "name") return null;
      const length = getOwnPropertyDescriptor(f, "length");
      const name = getOwnPropertyDescriptor(f, "name");
      for (const d of [length, name]) {
        if (!("value" in d) || d.writable || d.enumerable || !d.configurable) return null;
      }
      const out = ["F", name.value, length.value];
      const source = sourceDeviation(f, name.value);
      if (source !== null) out.push(source);
      return out;
    } catch {
      return null;
    }
  };

  const ids = new Map();
  const nodes = [];
  const queue = [];
  let head = 0;

  // A prototype is reached as a function's `prototype` or as an object's
  // [[Prototype]], or has a `constructor` whose `prototype` is itself.
  const isPrototype = (object) => {
    try {
      const d = getOwnPropertyDescriptor(object, "constructor");
      if (!d || !("value" in d) || typeof d.value !== "function") return false;
      const p = getOwnPropertyDescriptor(d.value, "prototype");
      return !!p && p.value === object;
    } catch {
      return false;
    }
  };

  const ref = (object, path, hint) => {
    let id = ids.get(object);
    if (id !== undefined) return id;
    id = nodes.length;
    ids.set(object, id);
    // An interface object's [[Prototype]] is its parent's interface object, a
    // function whose static getters still count.
    const kind =
      typeof object === "function"
        ? isPrototype(object)
          ? "x"
          : "f"
        : hint || (isPrototype(object) ? "x" : "i");
    nodes.push({ p: path, k: kind });
    queue.push([object, id]);
    return id;
  };

  const promises = [];
  const watch = (label, promise) => {
    const entry = [label, "pending"];
    promises.push(entry);
    try {
      apply(then, promise, [
        () => {
          entry[1] = "fulfilled";
        },
        (error) => {
          entry[1] = `rejected: ${error}`;
        },
      ]);
    } catch (error) {
      entry[1] = `then throws ${error}`;
    }
  };

  const encode = (value, path, hint) => {
    if (value === undefined) return ["u"];
    if (value === null) return ["null"];
    switch (typeof value) {
      case "boolean":
        return ["b", value];
      case "number":
        if (Object.is(value, -0)) return ["n", "-0"];
        return ["n", Number.isFinite(value) ? value : String(value)];
      case "string":
        return ["s", value.length > MAX_STRING ? `${value.slice(0, MAX_STRING)}...(${value.length})` : value];
      case "bigint":
        return ["i", String(value)];
      case "symbol":
        return ["y", String(value)];
      default:
        break;
    }
    // Objects, functions and document.all, whose typeof is "undefined".
    if (typeof value === "function") {
      const plain = plainFunction(value);
      if (plain) return plain;
    } else if (NodePrototype && value !== theDocument && apply(isPrototypeOf, NodePrototype, [value])) {
      let name;
      try {
        name = apply(nodeName, value, []);
      } catch (error) {
        name = `throws ${error}`;
      }
      return ["N", name];
    }
    if (nodes.length >= MAX_NODES) return ["cap"];
    return ["o", ref(value, path, hint)];
  };

  const slow = [];
  const throwing = [];

  const labelOf = (object, depth, node) => {
    try {
      const d = getOwnPropertyDescriptor(object, "constructor");
      if (d && "value" in d && typeof d.value === "function") return String(d.value.name);
    } catch {}
    return depth === 0 ? node.t : tagOf(object);
  };

  // What each getter on the object and its chain returns for it.
  const read = (object, node, chain) => {
    const seen = new Set();
    node.rd = [];
    let current = object;
    for (let depth = 0; current !== null && current !== undefined; depth++) {
      if (current === ObjectPrototype) break;
      let keys;
      try {
        keys = ownKeys(current);
      } catch {
        break;
      }
      const label = labelOf(current, depth, node);
      for (const key of keys) {
        if (seen.has(key)) continue;
        seen.add(key);
        let d;
        try {
          d = getOwnPropertyDescriptor(current, key);
        } catch {
          continue;
        }
        if (!d || !d.get) continue;
        const name = `${label}.${keyName(key)}`;
        if (SKIP.has(name)) {
          node.rd.push([keyName(key), ["skip"]]);
          continue;
        }
        const t0 = now();
        let value;
        let threw = false;
        try {
          value = apply(d.get, object, []);
        } catch (error) {
          threw = true;
          value = error;
        }
        const ms = now() - t0;
        takeReports(name);
        if (ms > 1) slow.push([name, round(ms)]);
        if (threw) {
          throwing.push([name, String(value)]);
          node.rd.push([keyName(key), ["E", String(value)]]);
          continue;
        }
        if (value !== null && (typeof value === "object" || typeof value === "function")) {
          try {
            if (apply(isPrototypeOf, PromisePrototype, [value])) watch(name, value);
          } catch {}
        }
        node.rd.push([keyName(key), encode(value, `${node.p}.${keyName(key)}`)]);
      }
      if (!chain) break;
      try {
        current = getPrototypeOf(current);
      } catch {
        break;
      }
    }
  };

  const visit = (object, id) => {
    const node = nodes[id];
    node.t = tagOf(object);
    if (typeof object === "function") {
      let name = null;
      let length = null;
      try {
        const n = getOwnPropertyDescriptor(object, "name");
        if (n && "value" in n) name = n.value;
        const l = getOwnPropertyDescriptor(object, "length");
        if (l && "value" in l) length = l.value;
      } catch {}
      node.f = [name, length];
      const source = sourceDeviation(object, name);
      if (source !== null) node.f.push(source);
    }
    try {
      const proto = getPrototypeOf(object);
      node.pr = proto === null ? null : encode(proto, `${node.p}.[[Prototype]]`, "x");
    } catch (error) {
      node.pr = ["E", String(error)];
    }
    let keys = [];
    try {
      keys = ownKeys(object);
    } catch (error) {
      node.oe = String(error);
    }
    node.own = [];
    for (const key of keys) {
      const name = keyName(key);
      let d;
      try {
        d = getOwnPropertyDescriptor(object, key);
      } catch (error) {
        node.own.push([name, "E", String(error)]);
        continue;
      }
      if (!d) continue;
      const path = `${node.p}.${name}`;
      if ("value" in d) {
        const hint = key === "prototype" && typeof object === "function" ? "x" : undefined;
        node.own.push([name, flags(d), encode(d.value, path, hint)]);
      } else {
        node.own.push([
          name,
          flags(d),
          d.get ? encode(d.get, `${path}<get>`) : null,
          d.set ? encode(d.set, `${path}<set>`) : null,
        ]);
      }
    }
    if (node.k === "i") read(object, node, true);
    else if (node.k === "f") read(object, node, false);
  };

  ref(globalThis, "globalThis", "i");
  while (head < queue.length) {
    const [object, id] = queue[head++];
    visit(object, id);
  }
  const walkMs = now() - started;

  // Calls whose results may follow the machine, each with a time limit.
  const calls = {};
  const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const plain = (value, depth = 0) => {
    if (value === null || value === undefined) return value === null ? null : "undefined";
    if (typeof value === "number" && !Number.isFinite(value)) return String(value);
    if (typeof value !== "object") return typeof value === "bigint" || typeof value === "symbol" ? String(value) : value;
    if (depth > 6) return "deep";
    if (Array.isArray(value)) return value.map((item) => plain(item, depth + 1));
    if (typeof value.toJSON === "function") {
      try {
        return plain(value.toJSON(), depth + 1);
      } catch {}
    }
    const out = {};
    for (const key in value) {
      if (typeof value[key] === "function") continue;
      try {
        out[key] = plain(value[key], depth + 1);
      } catch (error) {
        out[key] = `throws ${error}`;
      }
    }
    return out;
  };
  const call = async (name, f, limit = 3000) => {
    const t0 = now();
    try {
      const value = await Promise.race([
        Promise.resolve().then(f),
        sleep(limit).then(() => {
          throw new Error("no answer in time");
        }),
      ]);
      calls[name] = { v: plain(value), ms: round(now() - t0) };
    } catch (error) {
      calls[name] = { e: String(error), ms: round(now() - t0) };
    }
  };

  const day = Date.UTC(2026, 0, 15, 12);
  await call("Intl.DateTimeFormat().resolvedOptions()", () => new Intl.DateTimeFormat().resolvedOptions());
  await call("Intl.NumberFormat().resolvedOptions()", () => new Intl.NumberFormat().resolvedOptions());
  await call("Intl.Collator().resolvedOptions()", () => new Intl.Collator().resolvedOptions());
  await call("Intl.PluralRules().resolvedOptions()", () => new Intl.PluralRules().resolvedOptions());
  await call("Intl.RelativeTimeFormat().resolvedOptions()", () => new Intl.RelativeTimeFormat().resolvedOptions());
  await call("Intl.ListFormat().resolvedOptions()", () => new Intl.ListFormat().resolvedOptions());
  await call("Intl.Segmenter().resolvedOptions()", () => new Intl.Segmenter().resolvedOptions());
  await call("Intl.DurationFormat().resolvedOptions()", () => new Intl.DurationFormat().resolvedOptions());
  await call("Intl.DateTimeFormat timeZoneName long", () =>
    new Intl.DateTimeFormat(undefined, { timeZoneName: "long" }).format(day),
  );
  await call("Intl.supportedValuesOf counts", () =>
    Object.fromEntries(
      ["calendar", "collation", "currency", "numberingSystem", "timeZone", "unit"].map((key) => [
        key,
        Intl.supportedValuesOf(key).length,
      ]),
    ),
  );
  await call("Date", () => ({
    offsetJanuary: new Date(2026, 0, 15).getTimezoneOffset(),
    offsetJuly: new Date(2026, 6, 15).getTimezoneOffset(),
    toString: new Date(day).toString(),
    toLocaleString: new Date(day).toLocaleString(),
    toLocaleDateString: new Date(day).toLocaleDateString(),
    toLocaleTimeString: new Date(day).toLocaleTimeString(),
    number: (1234567.891).toLocaleString(),
  }));

  await call("matchMedia", () => {
    const features = {
      "prefers-color-scheme": ["light", "dark"],
      "prefers-reduced-motion": ["no-preference", "reduce"],
      "prefers-reduced-transparency": ["no-preference", "reduce"],
      "prefers-reduced-data": ["no-preference", "reduce"],
      "prefers-contrast": ["no-preference", "more", "less", "custom"],
      "forced-colors": ["none", "active"],
      "inverted-colors": ["none", "inverted"],
      "color-gamut": ["srgb", "p3", "rec2020"],
      "dynamic-range": ["standard", "high"],
      "video-dynamic-range": ["standard", "high"],
      pointer: ["none", "coarse", "fine"],
      "any-pointer": ["none", "coarse", "fine"],
      hover: ["none", "hover"],
      "any-hover": ["none", "hover"],
      orientation: ["portrait", "landscape"],
      "display-mode": ["browser", "minimal-ui", "standalone", "fullscreen", "window-controls-overlay", "picture-in-picture", "tabbed", "borderless"],
      scripting: ["none", "initial-only", "enabled"],
      update: ["none", "slow", "fast"],
      "overflow-block": ["none", "scroll", "paged"],
      "overflow-inline": ["none", "scroll"],
      grid: ["0", "1"],
      "environment-blending": ["opaque", "additive", "subtractive"],
      "device-posture": ["continuous", "folded"],
      monochrome: ["0"],
      "color-index": ["0"],
    };
    const out = {};
    for (const [feature, values] of Object.entries(features)) {
      out[feature] = values.filter((value) => matchMedia(`(${feature}: ${value})`).matches);
      if (!matchMedia(`(${feature})`).media.includes(feature)) out[feature] = "not parsed";
    }
    let color = null;
    for (let bits = 0; bits <= 48; bits++) if (matchMedia(`(color: ${bits})`).matches) color = bits;
    out.color = color;
    out["resolution = devicePixelRatio"] = matchMedia(`(resolution: ${devicePixelRatio}dppx)`).matches;
    out["width = innerWidth"] = matchMedia(`(width: ${innerWidth}px)`).matches;
    out["height = innerHeight"] = matchMedia(`(height: ${innerHeight}px)`).matches;
    out["device-width = screen.width"] = matchMedia(`(device-width: ${screen.width}px)`).matches;
    out["device-height = screen.height"] = matchMedia(`(device-height: ${screen.height}px)`).matches;
    return out;
  });

  await call("userAgentData.getHighEntropyValues", () =>
    navigator.userAgentData.getHighEntropyValues([
      "architecture", "bitness", "brands", "formFactors", "fullVersionList", "mobile", "model", "platform",
      "platformVersion", "uaFullVersion", "wow64",
    ]),
  );
  await call("getBattery", async () => {
    const b = await navigator.getBattery();
    return { charging: b.charging, chargingTime: b.chargingTime, dischargingTime: b.dischargingTime, level: b.level };
  });
  await call("storage.estimate", () => navigator.storage.estimate());
  await call("storage.persisted", () => navigator.storage.persisted());
  await call("enumerateDevices", async () =>
    (await navigator.mediaDevices.enumerateDevices()).map((d) => [d.kind, d.label === "", d.deviceId === "", d.groupId === ""]),
  );
  await call("speechSynthesis.getVoices", async () => {
    let voices = speechSynthesis.getVoices();
    if (voices.length === 0) {
      await new Promise((resolve) => {
        speechSynthesis.addEventListener("voiceschanged", resolve, { once: true });
        setTimeout(resolve, 2500);
      });
      voices = speechSynthesis.getVoices();
    }
    return voices.map((v) => [v.name, v.lang, v.localService, v.default]);
  }, 4000);
  await call("permissions.query", async () => {
    const names = [
      "accelerometer", "background-fetch", "background-sync", "camera", "captured-surface-control",
      "clipboard-read", "clipboard-write", "display-capture", "fullscreen", "geolocation", "gyroscope",
      "idle-detection", "keyboard-lock", "local-fonts", "local-network-access", "magnetometer", "microphone", "midi",
      "notifications", "payment-handler", "persistent-storage", "pointer-lock", "screen-wake-lock",
      "speaker-selection", "storage-access", "system-wake-lock", "top-level-storage-access", "web-app-installation",
      "window-management",
    ];
    const out = {};
    for (const name of names) {
      try {
        out[name] = (await navigator.permissions.query({ name })).state;
      } catch (error) {
        out[name] = `throws ${error.name}`;
      }
    }
    return out;
  });
  await call("keyboard.getLayoutMap", async () => {
    const map = await navigator.keyboard.getLayoutMap();
    const keys = ["KeyQ", "KeyW", "KeyA", "KeyY", "KeyZ", "KeyM", "Semicolon", "Quote", "BracketLeft", "Backquote", "IntlBackslash"];
    return { size: map.size, keys: keys.map((key) => [key, map.get(key) ?? null]) };
  });
  const decoding = (contentType) => ({
    type: "file",
    video: { contentType, width: 1920, height: 1080, bitrate: 5000000, framerate: 30 },
  });
  await call("mediaCapabilities.decodingInfo", async () => {
    const out = {};
    const types = {
      vp9: 'video/webm; codecs="vp09.00.10.08"',
      av1: 'video/mp4; codecs="av01.0.08M.08"',
      h264: 'video/mp4; codecs="avc1.640028"',
      hevc: 'video/mp4; codecs="hvc1.1.6.L120.90"',
    };
    for (const [name, type] of Object.entries(types)) {
      const info = await navigator.mediaCapabilities.decodingInfo(decoding(type));
      out[name] = [info.supported, info.smooth, info.powerEfficient];
    }
    return out;
  });
  await call("canPlayType and isTypeSupported", () => {
    const video = document.createElement("video");
    const types = [
      'video/mp4; codecs="avc1.42E01E"', 'video/mp4; codecs="hvc1.1.6.L93.B0"', 'video/mp4; codecs="hev1.1.6.L93.B0"',
      'video/mp4; codecs="av01.0.05M.08"', 'video/webm; codecs="vp9"', 'video/webm; codecs="vp8"',
      'audio/mp4; codecs="mp4a.40.2"', 'audio/webm; codecs="opus"', 'audio/flac', 'audio/mp4; codecs="ac-3"',
      'audio/mp4; codecs="ec-3"', 'audio/mp4; codecs="dtsc"', 'video/mp4; codecs="dvh1.05.06"',
      'audio/mpeg', 'audio/ogg; codecs="vorbis"', "application/vnd.apple.mpegurl", "video/x-matroska",
    ];
    return types.map((type) => [type, video.canPlayType(type), typeof MediaSource === "function" ? MediaSource.isTypeSupported(type) : null]);
  });
  await call("RTCRtpSender.getCapabilities", () => ({
    video: RTCRtpSender.getCapabilities("video").codecs.map((c) => `${c.mimeType} ${c.sdpFmtpLine || ""}`.trim()),
    audio: RTCRtpSender.getCapabilities("audio").codecs.map((c) => `${c.mimeType} ${c.clockRate}/${c.channels || 1}`),
  }));
  await call("RTCRtpReceiver.getCapabilities", () => ({
    video: RTCRtpReceiver.getCapabilities("video").codecs.map((c) => `${c.mimeType} ${c.sdpFmtpLine || ""}`.trim()),
  }));
  await call("chrome.loadTimes() and chrome.csi()", () => {
    const loadTimes = chrome.loadTimes();
    const csi = chrome.csi();
    return {
      loadTimesKeys: Object.keys(loadTimes),
      csiKeys: Object.keys(csi),
      connectionInfo: loadTimes.connectionInfo,
      npnNegotiatedProtocol: loadTimes.npnNegotiatedProtocol,
      wasFetchedViaSpdy: loadTimes.wasFetchedViaSpdy,
      wasNpnNegotiated: loadTimes.wasNpnNegotiated,
      wasAlternateProtocolAvailable: loadTimes.wasAlternateProtocolAvailable,
      navigationType: loadTimes.navigationType,
    };
  });
  await call("navigation entry", () => {
    const entry = performance.getEntriesByType("navigation")[0].toJSON();
    return { nextHopProtocol: entry.nextHopProtocol, type: entry.type, deliveryType: entry.deliveryType, keys: Object.keys(entry) };
  });
  await call("featurePolicy.allowedFeatures()", () => document.featurePolicy.allowedFeatures());
  await call("featurePolicy.features()", () => document.featurePolicy.features());
  await call("Error stack", () => {
    const stack = new Error("x").stack;
    return { stackTraceLimit: Error.stackTraceLimit, lines: stack.split("\n").length, first: stack.split("\n").slice(0, 2) };
  });
  await call("getGamepads", () => Array.from(navigator.getGamepads(), (g) => g && g.id));
  await call("bluetooth.getAvailability", () => navigator.bluetooth.getAvailability());
  await call("hid.getDevices", async () => (await navigator.hid.getDevices()).length);
  await call("usb.getDevices", async () => (await navigator.usb.getDevices()).length);
  await call("serial.getPorts", async () => (await navigator.serial.getPorts()).length);
  await call("xr.isSessionSupported", async () => ({
    vr: await navigator.xr.isSessionSupported("immersive-vr"),
    ar: await navigator.xr.isSessionSupported("immersive-ar"),
  }));
  await call("isUserVerifyingPlatformAuthenticatorAvailable", () =>
    PublicKeyCredential.isUserVerifyingPlatformAuthenticatorAvailable(),
  );
  await call("PublicKeyCredential.getClientCapabilities", () => PublicKeyCredential.getClientCapabilities());
  await call("AudioContext", async () => {
    const context = new AudioContext();
    const out = {
      sampleRate: context.sampleRate,
      baseLatency: context.baseLatency,
      outputLatency: context.outputLatency,
      maxChannelCount: context.destination.maxChannelCount,
      state: context.state,
    };
    await context.close();
    return out;
  });
  await call("gpu.getPreferredCanvasFormat", () => navigator.gpu.getPreferredCanvasFormat());

  // Promise-valued properties that have not settled after the calls.
  await sleep(500);
  takeReports("the calls");
  const totalMs = now() - started;

  return {
    meta: {
      walkMs: round(walkMs),
      totalMs: round(totalMs),
      nodes: nodes.length,
      readyState: document.readyState,
      url: location.href,
      skip: [...SKIP],
    },
    nodes,
    calls,
    reports,
    slow,
    throwing,
    promises,
  };
})();
