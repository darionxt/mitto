// What localStorage and sessionStorage do in a page at an HTTP origin, once it
// has loaded, which tests/test_page_storage.py runs in a Page that a Session
// opened and compares with what Chrome 157 gave for the same script, in
// tests/data/storage_cases.json. Each area starts empty, as on a new profile.
// Its value is a promise of the record.

(async () => {
  const out = {};
  const tick = (ms = 0) => new Promise((resolve) => setTimeout(resolve, ms));
  const outcome = (f) => {
    try {
      const value = f();
      return value === undefined ? "ok" : value;
    } catch (e) {
      return {
        name: e?.name,
        // A message that repeats a key of millions of characters is cut.
        message: e?.message?.slice(0, 120),
        constructor: e?.constructor?.name,
        quota: e?.quota,
        requested: e?.requested,
      };
    }
  };
  const keys = (storage) => {
    const all = [];
    for (let i = 0; i < storage.length; ++i) {
      all.push(storage.key(i));
    }
    return all;
  };
  const events = [];
  addEventListener("storage", (e) => events.push([e.key, e.newValue]));

  for (const [name, storage] of [
    ["local", localStorage],
    ["session", sessionStorage],
  ]) {
    const record = {};
    out[name] = record;
    record.kind = [
      typeof storage,
      Object.prototype.toString.call(storage),
      storage instanceof Storage,
      storage === (name === "local" ? localStorage : sessionStorage),
      storage.length,
    ];

    // What setItem() makes of its arguments, and what is not there.
    storage.setItem("n", 5);
    storage.setItem("u", undefined);
    storage.setItem(null, null);
    storage.setItem("", "empty key");
    record.values = [
      storage.getItem("n"),
      storage.getItem("u"),
      storage.getItem("null"),
      storage.getItem(""),
      storage.getItem("missing"),
      storage.key(1000),
      storage.key(-1),
      storage.length,
    ];
    storage.clear();

    // The order of key(): Blink's hash table, as it grows, shrinks and leaves
    // deleted buckets.
    const order = [];
    for (let i = 0; i < 30; ++i) {
      storage.setItem(`k${i}`, `${i}`);
    }
    order.push(keys(storage));
    for (let i = 0; i < 30; i += 3) {
      storage.removeItem(`k${i}`);
    }
    order.push(keys(storage));
    for (let i = 30; i < 50; ++i) {
      storage.setItem(`k${i}`, `${i}`);
    }
    order.push(keys(storage));
    for (let i = 0; i < 50; ++i) {
      if (i % 5 !== 0) {
        storage.removeItem(`k${i}`);
      }
    }
    order.push(keys(storage));
    storage.clear();
    for (const key of ["é", "ÿ", "日本", "😀", "\uD800", "a", "", "Z", "ab"]) {
      storage.setItem(key, key);
    }
    order.push(keys(storage).map((key) => [...key].map((c) => c.codePointAt(0))));
    storage.clear();
    for (let i = 0; i < 200; ++i) {
      storage.setItem(`key-${i * 7919}`, "");
    }
    order.push(keys(storage));
    record.order = order;
    storage.clear();

    // The quota: 10 MiB, two bytes for each character of keys and values.
    const half = 5 * 1024 * 1024;
    record.quota = [
      outcome(() => storage.setItem("big", "x".repeat(half))),
      storage.getItem("big"),
      outcome(() => storage.setItem("big", "x".repeat(half - 3))),
      storage.getItem("big")?.length,
      outcome(() => storage.setItem("y", "z")),
      storage.getItem("y"),
      outcome(() => storage.setItem("big", "x")),
      storage.getItem("big"),
      outcome(() => storage.setItem("é".repeat(half), "")),
      outcome(() => storage.setItem("", "日".repeat(half + 1))),
    ];
    storage.clear();

    // The named properties, and the names that the prototype has: a name
    // there is not intercepted, so setting it makes an own property, as
    // Chrome's interceptors do.
    record.named = [
      outcome(() => {
        storage.a = 1;
      }),
      storage.getItem("a"),
      "a" in storage,
      Object.keys(storage),
      JSON.stringify(Object.getOwnPropertyDescriptor(storage, "a")),
      delete storage.a,
      storage.getItem("a"),
      delete storage.missing,
      outcome(() => Object.defineProperty(storage, "b", { value: 2 })),
      storage.getItem("b"),
      outcome(() => {
        storage.length = 7;
      }),
      storage.getItem("length"),
      storage.length,
      outcome(() => {
        storage.setItem = "q";
      }),
      Storage.prototype.getItem.call(storage, "setItem"),
      typeof storage.setItem,
      Object.keys(storage),
      delete storage.setItem,
      typeof storage.setItem,
    ];
    storage.clear();
  }

  // A window hears of no change it makes itself.
  localStorage.setItem("mine", "1");
  sessionStorage.setItem("mine", "1");
  localStorage.clear();
  sessionStorage.clear();
  await tick(50);
  out.events = events;

  const made = new StorageEvent("storage");
  const given = new StorageEvent("storage", {
    key: "k",
    oldValue: "o",
    newValue: "n",
    url: "http://u/",
    storageArea: localStorage,
    bubbles: true,
  });
  const initialized = new StorageEvent("x");
  initialized.initStorageEvent("y", true, true, "k", null, "n", "v", sessionStorage);
  const show = (e) => [
    e.type,
    e.key,
    e.oldValue,
    e.newValue,
    e.url,
    e.storageArea === localStorage ? "local" : e.storageArea === sessionStorage ? "session" : e.storageArea,
    e.bubbles,
    e.cancelable,
    e.isTrusted,
  ];
  out.storage_event = [
    show(made),
    show(given),
    show(initialized),
    outcome(() => new StorageEvent("storage", { storageArea: {} })),
    outcome(() => StorageEvent("storage")),
  ];
  return out;
})();
