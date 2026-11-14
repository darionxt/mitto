---
status: accepted
---

# A Session keeps a page's storage in a directory of its own

Pages keep data in the browser: `localStorage` and `sessionStorage` on 80.6% of page loads, IndexedDB on 27.7% and the Cache API on 19.8%. Chrome's browser keeps all of it, apart from the cookies, and a renderer asks for it. A Session keeps its cookies and Machine in a file that the Python layer writes ([ADR-0007](0007-a-machine-of-its-own-per-session.md)), and a Page process opens no files ([ADR-0009](0009-a-process-for-each-page.md)). Storage does not fit that file: IndexedDB and the Cache API may each hold 60% of a disk, of structured values, blobs and whole responses, and IndexedDB writes each transaction when it commits.

## The decision

- **The Session runs it.** The native core keeps `localStorage`, `sessionStorage`, IndexedDB and the Cache API, as Chrome's browser and storage service do. The Page keeps Blink's side of each and asks over the channel, naming its document or worker, and the library works out the storage key from that, as Chrome's browser does. Two Pages of one Session share one state, as two tabs of a profile do.
- **Where it is kept.** `Session(storage=path)` names a directory, which the native core writes as it writes `cache=`. `localStorage` and IndexedDB go into SQLite, which is Chrome 157's store for both in memory and the store that its rollouts move both to on disk. The Cache API goes into `//net`'s disk cache, the simple backend, as Chrome has it. An IndexedDB transaction is written when it commits, and `localStorage` in Chrome's batches. One Session at a time has the directory, in any process.
- **Without a directory.** The same stores in memory until the Session closes, as Chrome's Incognito keeps them, with Incognito's quota, drawn from the Machine's memory and held below the Host's.
- **Its Machine.** The directory names the Machine whose storage it is. A Session that opens on it claims that Machine, and a Session of another Machine fails to open, as with `cookie_file`.
- **What is not saved.** `sessionStorage` lives as long as its Page, since no later Session restores a Page.
- **The cookie file** stays at version 2.

## Considered Options

- **Version 3 of the cookie file**, with storage beside the cookies and the Machine, written by Python. One file, one atomic save and no new dependency. But all of it stays in memory and is written whole at each save, with binary values as base64, a crash loses everything since the last save, `Cookies.save()` would have to write storage too, and mitto 0.2 and 0.3 could not read the file.
- **The Page runs the engines, and the Session keeps bytes.** Fewer messages, but each Page would have its own copy, with no `storage` events or `versionchange` between Pages, and the last save would win.
- **Chrome's storage service and IndexedDB as they are.** They are built on Mojo, `//content` and the quota manager, which the native core does not have. Their code is ported instead, as Blink's is.
- **LevelDB, Chrome's default on disk today.** It would match the files of a Chrome profile, which no page sees, and Chrome is moving off it. SQLite also serves the stores in memory, with one dependency.
- **Saving `sessionStorage`.** Nothing would read it, since a Page is not restored, and Chrome clears it at a start after a clean exit.

## Consequences

- SQLite and `//sql` join the build and the source export, with slices on every Host.
- A caller who wants storage kept names a directory beside `cookie_file`. Until the Runtime ships, it stays private, as `Session(_storage=path)`, and no release writes anything new.
- `estimate()` reports the usage plus 10 GiB, and the usage counts what mitto's stores hold, which differs from what Chrome's LevelDB files hold. Nothing is evicted.
- IndexedDB follows the lock scheme and value wrapping of Chrome's store on disk, not Incognito's, whatever holds its bytes.
- The Page's messages gain a family for each kind, each with a case in `page_messages_fuzzer`.
- The roadmap's M8 line no longer saves `sessionStorage`.
