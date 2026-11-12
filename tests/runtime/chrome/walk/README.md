# A throwaway walk for #207

This branch is never merged. It holds what answered darionxt/mitto#207, "What real Chrome's Script Fingerprint depends on".

| File | What it does |
| --- | --- |
| `walk.js` | Walks everything a page reaches from `globalThis`: each object's own properties in order, with their attributes, functions' names, `length`s and source text when it is not `[native code]`, prototype chains, and what each getter returns on objects that are not prototypes. Then it makes some calls whose results may follow the machine, such as `matchMedia()`, `Intl`'s defaults and `getHighEntropyValues()`, and records deprecation reports, slow and throwing getters and promises that never settle |
| `walk.html` | Runs `walk.js` once the page has loaded, through an indirect eval as `record.html` runs the probe, and posts the result to `result?run=NAME` |
| `walk_server.py` | Serves the page over HTTP and HTTPS on loopback, with a private certificate for `capture.test`, and writes each result to a file. Standard library only |
| `walk.ps1` | On the Windows machine: starts Chrome through a scheduled task on the logged-in desktop for each run of a phase, waits for the result, reads `chrome://version/?show-variations-cmd` in another tab when the run has a debugging port, and closes Chrome through its window |
| `compare.py` | What differs between two walks, matching objects by the path at which the walk found them, or each walk's size and oddities with `--summary` |
| `extras.py` | What a walk has that the bindings generator's `web_idl_listing.txt` does not |
| `seed_surface.py`, `seed_read_lib.py` | The studies of a variations seed that switch a feature a page can see |

The phases of `walk.ps1`:

| Phase | Chrome | Runs |
| --- | --- | --- |
| a | 157.0.8103.39, installed | A first start with no seed; after the seed: twice with the same flags, with `--remote-debugging-port=9207` and `=0`, with `--ignore-certificate-errors-spki-list`, at `https://capture.test`, at `http://capture.test`, with `--host-resolver-rules` at `127.0.0.1`, started through a shortcut, with `start /affinity`, with `--force-device-scale-factor`, with `--window-size` and `--window-position` |
| b | 157.0.8103.39, installed | A second user data directory, first start and seeded |
| c | 155.0.8059.85, unpacked | First start, seeded, and seeded at `https://capture.test` |
| d | 156.0.8078.80, unpacked | First start and seeded |
| e | 157.0.8103.39, unpacked | First start and seeded |

Every start has `--user-data-dir`, `--no-first-run`, `--no-default-browser-check`, `--lang=en-US` and `--variations-override-country=us`.
