# Changelog

What changes for callers in each version of mitto, newest first. The notes of each version's GitHub release hold its entry, and [releasing.md](docs/releasing.md#what-changes-for-callers) says how one is written.

## 0.2.0

Each Session claims a **Machine** of its own: the memory, display, window, network, user settings and Windows version that its client hints say, once a server asks for them. In mitto 0.1, every Session of a Persona sent the same values. A server that does not ask for these hints sees no change.

- A Session draws its Machine from a table that ships with mitto, unless [`machine=`](docs/api.md#machine) gives one, and [`Session.machine`](docs/api.md#sessionmachine) is the one it claims. [`Machine.draw()`](docs/api.md#machinedraw) draws one with any value fixed, and the same one again from the same seed with the same table. Each run of the release routine refreshes the table, so a later version can draw another Machine from that seed. [`Machine.named("mitto-0.1")`](docs/api.md#machinenamed) is the one that every Session of mitto 0.1 claimed.
- `sec-ch-ua-platform-version` is no longer `"19.0.0"` for every Session: a Session sends `"10.0.0"` for a Machine on Windows 10, and `"19.0.0"` for one on Windows 11.
- `rtt` and `downlink` carry [Chrome's noise for each host](docs/persona.md#the-noise-on-rtt-and-downlink), so they change from host to host, and often between a navigation and the scripts, images and fetches of its page. A Session draws the noise when it opens, from its Machine's seed, so that the same Machine sends the same values again. A Machine with no seed, such as one a caller makes or `Machine.named("mitto-0.1")`, gets new noise each time a Session opens, as Chrome does each time it starts. So `Machine.named("mitto-0.1")` sends a `downlink` from 9 to 10, by host, where mitto 0.1 sent 10 to every host.
- A [`cookie_file`](docs/api.md#cookie_file), and a file that [`Cookies.save()`](docs/api.md#cookiessave) writes, holds the Session's Machine beside its cookies, as version 2 of the file, and a Session that opens with it claims that Machine. mitto 0.1 refuses a file of version 2, so a caller that may go back to mitto 0.1 needs a copy of the file from before. A file from mitto 0.1 loads as `Machine.named("mitto-0.1")`, and `close()` writes it again as version 2. A `machine=` that differs from the file's Machine raises a `ValueError`, and so does [`Cookies.load()`](docs/api.md#cookiesload) of a file of another Machine.
- The wheel declares its licence as `Apache-2.0 AND CC-BY-SA-3.0`. The table of Machines is under CC BY-SA 3.0, with the credits of its sources in [`MACHINE_TABLE_NOTICE`](MACHINE_TABLE_NOTICE), and the code stays under Apache-2.0.

## 0.1.0

The first version.
