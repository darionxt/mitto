// What a response's Permissions-Policy and Feature-Policy headers give its document and that document's frames. The page is at
// /app/page, whose own header is `microphone=(), unload=*, usb=(self "OTHER")`, and /with?HEADERS answers with the headers that its query
// names, 66,000 letters for LONG in them, and runs tests/policy_headers_child.js. The value is a promise for a record by the headers, which
// tests/data/policy_headers_cases.json holds as Chrome gave it.
(async () => {
  const OTHER = `http://localhost:${location.port}`;
  const plain = (value) => JSON.parse(JSON.stringify(value).replaceAll(OTHER, 'OTHER').replaceAll(location.origin, 'ORIGIN'));
  const ask = async (headers, allow = '') => {
    const element = document.createElement('iframe');
    element.allow = allow;
    element.src = `/with?${encodeURIComponent(JSON.stringify(headers))}`;
    const heard = new Promise((resolve) => addEventListener('message', function listener({source, data}) {
      if (source !== element.contentWindow) return;
      removeEventListener('message', listener);
      resolve(data);
    }));
    document.body.append(element);
    const value = await Promise.race([heard, new Promise((resolve) => setTimeout(resolve, 10000, 'never'))]);
    element.remove();
    return value;
  };
  const permissions = ['camera=()', 'camera=self', 'camera=*', 'camera=(self)', 'camera=(self "https://a.example")', 'camera="https://a.example"', 'camera=("https://*.b.example")', 'camera=("https://*.b.example" self "http://c.example:8080" "https://a.example")',
    'camera=(self "OTHER")', 'camera=("https://*")', 'camera=("https://*:*")', 'camera=("http://c.example:*")', 'camera=("https:")', 'camera=("http:")', 'camera=(*)', 'camera=(* self)', 'camera=(src)', `camera=("'self'")`, `camera=("*")`, 'camera=(1)', 'camera=1', 'camera',
    'camera=?0', 'camera=(self);report-to="x"', 'camera=(self;a=1)', 'camera=self;a=1', 'camera=(), camera=*', 'camera=*, gamepad=()', 'Camera=()', 'unknown=()', 'camera=(', 'camera=();', 'camera=() gamepad=()', '', 'web-share=*',
    'camera=("nonsense")', 'camera=("https://a.example/path?q")', 'camera=("HTTPS://A.EXAMPLE:443")', 'camera=("")', 'camera=("https://\\"")', 'gamepad=self', 'gamepad=()', 'gamepad=(self "OTHER")', 'geolocation=*', 'fullscreen=()', 'picture-in-picture=()',
    'cross-origin-isolated=()', 'cross-origin-isolated=*', 'local-network-access=()', 'local-network=(), local-network-access=self', 'microphone=*', 'usb=*', 'camera=:AA==:', 'camera=(self none)', 'camera=(none)', 'camera=none', 'ch-ua=()', 'focus-without-user-activation=()', 'camera=()\f', '\vcamera=()', 'camera=()\t '];
  const feature = [`camera 'none'`, `camera 'self'`, 'camera *', 'camera', `camera 'src'`, `camera https://a.example`, `camera https://*.b.example`, `camera 'none', gamepad 'self'`, `camera 'none'; gamepad 'self'`, 'unknown', `camera 'none'; camera *`, 'camera nonsense', `CAMERA 'none'`, ';;,', `gamepad OTHER`, `camera\f'none'`, `camera 'none'\v`];
  const record = {page: {}};
  const own = document.featurePolicy;
  for (const name of ['microphone', 'usb', 'camera']) record.page[name] = [own.allowsFeature(name), own.allowsFeature(name, OTHER), own.getAllowlistForFeature(name)];
  record.none = await ask({});
  const base = record.none;
  // Only what differs from a document of no header.
  const differing = (value) => {
    if (typeof value === 'string') return value;
    const result = {};
    for (const name in value.names) if (JSON.stringify(value.names[name]) !== JSON.stringify(base.names[name])) result[name] = value.names[name];
    for (const kind in value.frames) {
      const [inherited, count, element] = value.frames[kind], [baseInherited, baseCount, baseElement] = base.frames[kind];
      const of = (now, before) => Object.fromEntries(Object.entries(now).filter(([name]) => JSON.stringify(now[name]) !== JSON.stringify(before[name])));
      const changed = [of(inherited, baseInherited), count - baseCount, of(element, baseElement)];
      if (JSON.stringify(changed) !== '[{},0,{}]') result[kind] = changed;
    }
    if (value.count !== base.count) result.count = value.count - base.count;
    if (value.fullscreen !== base.fullscreen || value.pip !== base.pip) result.enabled = [value.fullscreen, value.pip];
    return result;
  };
  const fill = (text) => text.replaceAll('OTHER', OTHER);
  for (const header of permissions) record[`Permissions-Policy: ${header}`] = differing(await ask({pp: [fill(header)]}));
  for (const header of feature) record[`Feature-Policy: ${header}`] = differing(await ask({fp: [fill(header)]}));
  for (const [name, headers] of Object.entries({
    'both': {pp: ['camera=()'], fp: [`camera *; gamepad 'none'`]}, 'both, two overlaps': {pp: ['camera=(), gamepad=self'], fp: [`gamepad *; camera *`]}, 'two headers': {pp: ['camera=()', 'gamepad=()']}, 'two headers of one': {pp: ['camera=()', 'camera=*']},
    'two headers, one bad': {pp: ['camera=()', 'gamepad=(']}, 'two feature headers': {fp: [`camera 'none'`, `camera *; gamepad 'none'`]}, 'both bad': {pp: ['camera=('], fp: ['unknown *']}, 'too long': {pp: ['camera=(), gamepad="LONG"']}, 'too long a feature header': {fp: [`camera 'none'; LONG`]},
  })) record[name] = differing(await ask(headers));
  // unload, whose default allowlist is none on the page's host, as each frame's document and its element have it.
  for (const [header, allow] of [[null, 'unload'], ['unload=*', 'unload'], ['unload=self', 'unload'], ['unload=()', 'unload'], ['unload=("OTHER")', 'unload'], ['unload=*', '']]) {
    record[`unload: ${header}, allow="${allow}"`] = await ask({unload: true, pp: header ? [fill(header)] : []}, allow);
  }
  return plain(record);
})()
