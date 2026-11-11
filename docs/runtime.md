# The Runtime

The **Runtime** runs a **Page**'s scripts in a process of its own, on the **Engine Release**'s V8, while the Page's **Session** stays in the caller's process with its network, cookies and cache ([ADR-0009](adr/0009-a-process-for-each-page.md)). Terms in **bold** are defined in [CONTEXT.md](../CONTEXT.md). [building.md](building.md#build-the-runtime) says how the Page program is built. Until the release that ships the Runtime, the Page API is private, and a wheel that a release makes holds no Page program.

## A Page and its process

Each Page is a process of the Page program, `mitto_page`, or `mitto_page.exe` on Windows. `mitto_page_open()` in the C API starts it from the directory that the library was loaded from, which is where a wheel puts both. Without a program there, as in a wheel that a release made, it fails with `ERR_FILE_NOT_FOUND`.

The library makes the channel first, then starts the program with its end of the channel and one argument, `--session=CHANNEL,PARENT`. `CHANNEL` names that end and `PARENT` is the ID of the process that holds the library. The program's standard input is the null device, and it shares standard output and error with the process that holds the library, so that what V8 or a sanitizer reports on a crash is not lost. It inherits nothing else:

| Host | The channel | What else the Page process inherits | The Page ends when the process that holds the library ends |
| --- | --- | --- | --- |
| macOS | One end of a socket pair, at descriptor 3 | Nothing: `posix_spawn()` closes every descriptor that it is not told to pass on | Its end of the channel reads the end of the stream. It also watches its parent with a `kqueue`, for when another process holds the library's end, such as one that the parent forked |
| Linux | One end of a socket pair, at descriptor 3 | Nothing: `//base` closes every other descriptor before it starts the program, and sets `PR_SET_NO_NEW_PRIVS` | The end of the stream, and the kernel's `PR_SET_PDEATHSIG`, which kills it when the thread that started it ends. So the library starts each Page from a thread that never ends |
| Windows | The client end of a named pipe, whose handle value the argument holds | Nothing: only that handle is in `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` | The pipe breaks. It also runs in a job object of its own, with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, which ends it when the last handle to the job closes, as when the process that holds the library ends |

On Windows the job also has `JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION`, so that a crash ends the process at once rather than waiting for Windows Error Reporting, and the program starts hidden, so that a caller with no console gets no window. The named pipe has a random name under `\\.\pipe\LOCAL\`, one instance, and remote clients refused. Its end in the library is not inherited by any program, and the library opens the client end itself before it starts the Page, as Mojo's `PlatformChannel` does.

On Linux, a Page process holds about 18 MB of memory before a page loads. Besides its main thread and its I/O thread it runs V8's worker threads, one fewer than the machine has cores.

## What a Page process holds and reads

A Page process holds its end of the channel and its standard streams, and opens no file or socket of its own, so that Chromium's sandbox can hold it before the Runtime ships (roadmap, M10):

- ICU's whole data file and V8's startup snapshot are inside the program ([building.md](building.md#icus-data-and-v8s-snapshot)).
- The time zone and the locale come from the Session, in the start message below, so ICU never reads `TZ`, `/etc/localtime` or the zone files. The options of `mitto_page_open()` give them, and default to `UTC` and `en-US`. The Page sets them as Blink sets the time zone that its browser reads.
- V8's random numbers come from the system's random source through `//base`, which reads no file, as in Chrome's renderers.

What it holds is checked on Linux, where a test reads `/proc/<pid>/fd` of a Page that has run a script. It finds standard input on `/dev/null`, standard output and error as the test program's, the socket at descriptor 3, and the epoll and eventfd of the program's I/O thread, which name no file.

Before its `main()`, the dynamic loader of Linux opens the shared libraries of glibc, as it does for any program. After it, `strace` shows the Page reading `/sys/devices/system/cpu/online`, for how many threads V8 starts, and `/proc/self/maps`, which glibc reads to find the main thread's stack, and opening nothing else. Chrome's renderers read the same, and the sandbox of M10 has to allow them, as Chrome's does.

## Inside the Page program

The program runs V8 on its main thread, and reads the channel on a thread of its own. The I/O thread checks each message, hands scripts to the main thread in the order they came, and sends their results back. Because it reads while a script runs, the process ends as soon as the channel closes, even in the middle of a script that never ends.

Each script runs in one context, so each sees what the ones before it left there. Its result is what `JSON.stringify()` writes for it, nothing for a value that it writes nothing for, such as `undefined` or a function, and what was thrown, as `Uncaught TypeError: ...`, for a script that throws or a result that cannot be written as JSON, such as a BigInt. When V8 runs out of memory, the program ends with exit code 3, which the library reports as running out of memory.

Given a script on its command line instead of `--session=`, the program runs it and prints the result, which [building.md](building.md#build-the-runtime) describes. Either way, the context has the interfaces that the Runtime implements, as the Page's runtime-enabled features expose them ([the bindings](#the-bindings)).

## The channel

The channel is the native core's own, on `//base`, and not Chromium's Mojo ([ADR-0009](adr/0009-a-process-for-each-page.md)). In the library, the network thread reads and writes it, on its message pump: with a watch on the socket on macOS and Linux, and with overlapped reads and writes on the pipe, which complete on the thread's completion port, on Windows. The Page program does the same on its I/O thread. Neither ever blocks on it: what the system cannot take yet waits in memory.

Each message travels as a frame: the size of its payload and its type, each a 32-bit little-endian number, and then the payload. Each side refuses a frame whose payload is larger than 16 MiB, so that the other cannot make it hold more for one message. So a script can be at most 16 MiB less 8 bytes, and a result whose JSON is larger fails with a message that says so.

[`native/page_messages.h`](../native/page_messages.h) holds the frames and the messages, which both programs build. [`native/page_channel.h`](../native/page_channel.h) holds the channel, with one file for macOS and Linux and one for Windows.

## The messages

Version 1 of the messages:

| Type | Name | Way | Payload |
| --- | --- | --- | --- |
| 1 | Hello | Each way, first and once | The version of the messages that the sender speaks, a 32-bit number |
| 2 | Refused | Each way | Why the sender refuses what it got, in UTF-8. The sender closes the channel after it |
| 3 | Start | To the Page, once, after the hello | A JSON object with `time_zone` and `locale`, and no other key |
| 4 | Evaluate | To the Page | The number of the call, a 64-bit number, and the script in UTF-8 |
| 5 | Evaluated | To the Session | The number of the call, a byte that is 0 for a result and 1 for a failure, and then the result as JSON, or why it failed, in UTF-8 |

The library sends its hello, the start and any script at once, without waiting for the Page's hello, because the Page reads them in order. Bodies and the credit that limits them come with the Page's requests, in later versions.

Both programs come from one tree and ship together, so the version on both sides must be the same, and it changes with any change to the messages. Each side refuses, with a Refused message that says why:

- a hello of another version, and any message before the hello;
- a type that it does not know, or one that only goes the other way;
- a payload that is not what its type says, such as text that is not UTF-8;
- a frame larger than the channel carries;
- for the library, a result of a call that is not in progress. For the Page, a second start, a script before the start, or a time zone or locale that ICU does not know.

The library reads what a Page sends as hostile input: a Page runs a site's scripts. It checks each frame field by field before it acts on it, and the fuzz target `page_messages_fuzzer` feeds any bytes to that code ([building.md](building.md#sanitizer-builds-and-the-fuzz-target)). When the library refuses, it also kills the Page process, rather than trusting it to end.

A refusal ends the Page, and its calls fail with `MITTO_PAGE_ERROR_REFUSED` and a message that says which side refused what, such as:

```text
the library refused a message from the Page: the Page speaks version 99 of the messages, and the library speaks 1
the Page refused a message from the library: the time zone "Mars/Olympus_Mons" is unknown
```

## The C API

[`mitto.h`](../native/include/mitto/mitto.h) has the Page's calls, which return at once, and its events, which wait in a native queue as a request's do ([ADR-0003](adr/0003-c-abi-and-ctypes.md)):

- `mitto_page_open()` opens a Page on a Session, with versioned JSON options. An unknown key is refused.
- `mitto_page_evaluate()` sends a script and stores the number of its call. Each call gets one `MITTO_PAGE_EVENT_EVALUATED` event, with the result as JSON or an error.
- `mitto_page_next_event()` takes the next event, blocking for as long as the caller says, and `mitto_page_event_free()` releases it.
- `mitto_page_process_id()` gives the ID of the Page's process.
- `mitto_page_close()` closes the Page.

A Page's events list its handle in the Session's list of what is ready, beside its requests' events. So the file descriptor that an asyncio caller watches on macOS and Linux, and `mitto_session_wait_for_event()` on Windows, wake for both. `mitto_session_next_ready()` names the handle and says whether it is a request's or a Page's.

## How a Page ends

Each way of ending reaps the process, so none is left behind:

- `mitto_page_close()` closes the channel. The Page process reads the end of the stream and ends at once. The call waits for that, for up to 2 seconds before it kills the process, and returns once the process has gone.
- `mitto_session_destroy()` does the same for each Page of the Session. Each Page gets a `MITTO_PAGE_EVENT_ENDED` event with `MITTO_PAGE_ERROR_SESSION_DESTROYED`, and its handle stays valid until it is closed.
- When the process that holds the library ends without either, the Page process ends as the table above says.
- When the Page process crashes, runs out of memory or is killed, the library reads the end of the stream on the network thread, and a worker thread reaps the process to learn how it ended. Each call in progress then fails with `MITTO_PAGE_ERROR_PROCESS_ENDED`, a `MITTO_PAGE_EVENT_ENDED` event follows, and each later call fails the same way. The message says how the process ended, such as `the Page process was ended by signal 9` or `the Page process ran out of memory`. The Session's other Pages and requests go on.

On macOS and Linux the library reaps its Page processes with `waitpid()` on the process's ID. A caller that waits for any child, or ignores `SIGCHLD`, can take a Page's exit status first. The library then says that the process ended and that something else in the process took its exit status.

## The bindings

What a page's scripts can reach of the Web's interfaces, and how, is part of its **Script Fingerprint**. So the Runtime's V8 bindings come from the Engine Release's Web IDL files, as Chrome's do, and are written by a generator of mitto's own, [`native/bindings/bindings_generator`](../native/bindings/bindings_generator), which GN runs on every build of the Runtime. Blink's own generator, `bind_gen`, writes code for Blink's classes, which mitto does not have. The generator follows `bind_gen` for everything a page can see: which properties each object has and in which order, their attributes, names and `length`s, and the errors that each call throws, with Chrome's messages. Each module names the files of Blink's that it follows on its `Upstream:` lines.

### How the generator works

It runs in four steps, all in [`native/bindings/generate_bindings.py`](../native/bindings/generate_bindings.py) and the package next to it:

1. **Read.** Blink's scripts build the [Web IDL database](building.md#the-web-idl-database) from the IDL files of every interface that Chrome exposes to pages, with their partial interfaces and mixins merged in. The generator loads it with Blink's own `web_idl` package and reads every definition into a model of its own ([`web_idl_reader.py`](../native/bindings/bindings_generator/web_idl_reader.py), [`model.py`](../native/bindings/bindings_generator/model.py)): interfaces, namespaces, dictionaries, callbacks, enumerations, and each of their members, types and extended attributes. It reads all of them, whether or not the Runtime implements them yet, and it refuses anything it does not know, with a message that names it and where it is: a kind of definition, a type, or an extended attribute that [`extended_attributes.py`](../native/bindings/bindings_generator/extended_attributes.py) does not list. That file gives each of the 73 extended attributes that the database of 157.0.8103.39 uses a role: what it changes for a page, or that only Blink's implementation reads it, such as `[Measure]`. So when an Engine Release brings a construct that nobody has looked at, the build stops with its name, instead of writing bindings that are wrong.
2. **Plan.** For every interface, [`install_plan.py`](../native/bindings/bindings_generator/install_plan.py) works out the own properties of its interface object, prototype and instances in the order that Chrome installs them. `bind_gen` installs the members that are always there on V8's templates first, then those behind a runtime-enabled feature, and later, on each context's objects, those that the context decides, such as `[SecureContext]` members. V8 adds `constructor` to the prototype between the two. Within each of these, Blink installs the attributes, then the constants, then the operations, which `web_idl` sorts by name, with the members of one condition together. `[LegacyUnforgeable]` members go on each instance, not configurable.
3. **Write.** For each interface that has a C++ class in the Runtime, [`cpp_writer.py`](../native/bindings/bindings_generator/cpp_writer.py) writes `v8_<interface>.h` and `.cc` into `out/runtime/gen/mitto/bindings`: a callback for each attribute, operation and constructor, and the function that installs them on the interface's templates in the order of the plan. It also writes the dictionaries and unions those use, the table of runtime-enabled features, and the function that puts the interface objects on a Page's global object, in the order of Chrome's `Window`. It converts values as Web IDL says, in the order of `bind_gen`'s callbacks: V8's signature check of the receiver, which throws "Illegal invocation"; then for a constructor that it is called with `new`; then the number of arguments; then each argument in turn ([`cpp_types.py`](../native/bindings/bindings_generator/cpp_types.py)). It handles only the types and extended attributes that those interfaces use, and refuses any other there, naming it: adding an interface may mean teaching it more.
4. **List.** It writes `web_idl_listing.txt`, every interface and member that it planned, one per line, with the features and conditions each is behind. The release routine compares it between two Engine Releases ([releasing.md](releasing.md#2-check-out-the-new-engine-release)).

The interfaces with a C++ class are those in [`native/bindings/interfaces.gni`](../native/bindings/interfaces.gni), and until M7 brings bindings for every interface (roadmap, M7), only those are exposed to a page. Each class is `mitto::<interface>` in [`native/dom`](../native/dom). A member whose behaviour the Runtime lacks is a **Known Gap**, which `missing` lists there: its binding is installed as Chrome's is, so the shape matches, and it throws `TypeError: Not implemented.` when a page calls it. Today that is `EventTarget.prototype.when()`, whose `Observable` the Runtime does not have.

For Chromium 157.0.8103.39, the generator reads the whole database in about a second, and the listing has 18,069 lines, about 1.1 MB.

### What the bindings run on

The generated callbacks call a small runtime in [`native/bindings`](../native/bindings), on V8's own API, as Blink's `platform/bindings`:

- The Runtime's C++ objects live on V8's C++ heap, cppgc, on which Blink's Oilpan is built, so that V8 collects them with the JavaScript objects that refer to them ([`script_wrappable.h`](../native/bindings/script_wrappable.h)). A wrapper holds its object through `v8::Object::Wrap()`, with the type tag that Blink's Web IDL database gives the interface, and the object holds its wrapper through a traced reference. A listener that a page adds is held the same way, so a target, its listeners and their closures go together when nothing else holds them.
- An argument converts to an object of an interface only if V8's template of that interface, or of one that inherits from it, made it, and its tag lies in the interface's range.
- An error's message is the error itself, such as "1 argument required, but only 0 present.". As in Chrome, the context of the call goes in front of it as it leaves the call, through V8's exception propagation callback ([`exception_context.h`](../native/bindings/exception_context.h)): "Failed to execute 'addEventListener' on 'EventTarget': ", and for each dictionary being converted, "Failed to read the 'signal' property from 'AddEventListenerOptions': ". V8 calls it for an error thrown directly in the call, so an error that V8 throws there, such as converting a Symbol to a string, gets the context too, and one that a page's own getter throws does not.

### Runtime-enabled features

Blink puts many interfaces and members behind runtime-enabled features, which [`runtime_enabled_features.json5`](https://source.chromium.org/chromium/chromium/src/+/main:third_party/blink/renderer/platform/runtime_enabled_features.json5) lists. A Page has a set of features when it starts, and installs what is behind each one when it builds its templates, as Blink does, so that M7 can give a Persona of an older Chrome Release the set of its release ([ADR-0008](adr/0008-v8-and-bindings-from-web-idl.md)).

By default a Page has the Engine Release's set for stable on Windows, which the generator works out from that file as Blink's build and Chrome's startup work it out ([`runtime_features.py`](../native/bindings/bindings_generator/runtime_features.py)): a feature is on if its status for Windows is `stable`, or if the base feature that Chrome copies into it is on by default. When a page's binding asks, a feature that it depends on and that is off turns it off, and one that implies it and is on turns it on. What Chrome's own code turns on beside the file, and field trials, are not counted, and M7's Baselines show where that differs.

The Page program takes Chrome's options for the set, before the script or `--session=`: `--enable-blink-features=A,B` and `--disable-blink-features=C`, each a list of names separated by commas, which turn those features on and then off, so a feature that both name ends up off, as in Chrome. It refuses a name that is not a feature. A Page that the library starts gets the stable set, until the start message carries the same two lists for a Persona's own set (M7).

```text
$ out/runtime/mitto_page "'hasUAVisualTransition' in PopStateEvent.prototype"
true
$ out/runtime/mitto_page --disable-blink-features=HasUAVisualTransition "'hasUAVisualTransition' in PopStateEvent.prototype"
false
```

### Add an interface

1. Write its class in `native/dom/<file>.h` and `.cc`, where `<file>` is the interface's name in lowercase words, such as `pop_state_event` for `PopStateEvent`: `mitto::<interface>`, which derives from `bindings::ScriptWrappable`, or from its parent interface's class, and returns its generated `bindings::V8<interface>::kWrapperTypeInfo` from `GetWrapperTypeInfo()`. It has a static `Create()` with the constructor's arguments, if the interface has one, and a method for each member, named after the member, or after its `[ImplementedAs]`, with its first letter in upper case: `Type()` for an attribute `type`, `SetCancelBubble()` for its setter, and `InitEvent()` for an operation `initEvent`. A method takes the arguments of the IDL, converted: `bool`, `const std::u16string&` for a `DOMString`, a pointer to the class of an interface, a generated struct for a dictionary, `std::optional` for an optional argument without a default. One whose member is `[RaisesException]` takes a `bindings::ExceptionState&` last.
2. Add it to `mitto_bindings_interfaces` in [`native/bindings/interfaces.gni`](../native/bindings/interfaces.gni), with any member whose behaviour it does not have yet in `missing`, and its files to [`native/dom/BUILD.gn`](../native/dom/BUILD.gn). Its parent interface needs a class first.
3. Build the Runtime. If the C++ writer refuses a type, an extended attribute or a construct that the interface uses, teach it in `cpp_types.py` or `cpp_writer.py`, following `bind_gen` for what a page sees.
4. Add the interface to the list in [`tests/runtime/chrome/probe.js`](../tests/runtime/chrome/probe.js), with an instance and any calls worth comparing, and to `_INTERFACES` in [`tests/test_page_bindings.py`](../tests/test_page_bindings.py). Then [record](#what-the-tests-compare-with-chrome) the Engine Release's Chrome again and run the tests.

### What the tests compare with Chrome

[`tests/test_page_bindings.py`](../tests/test_page_bindings.py) runs [`tests/runtime/chrome/probe.js`](../tests/runtime/chrome/probe.js) in the Page program, and compares what it records with what the same probe recorded in real Chrome of the Engine Release, which [`tests/runtime/chrome`](../tests/runtime/chrome) keeps in a file for each Chrome build, such as `157.0.8103.39.json`. The probe records the own properties of each interface the Runtime implements, of its interface object, its prototype and an instance, in order, with their attributes, names and `length`s, the parent of its prototype, what each member throws when it is called on an object of another kind, and the outcome of some calls, such as a dispatch to listeners or a call with an argument of the wrong type. It records interfaces, descriptors, lengths and messages only, which say nothing of the machine that ran it. M7's Baselines will replace these files and the step that records them.

To record one, on the Windows machine with the Engine Release's Chrome installed, as [baselines.md](baselines.md#install-chrome) installs it, and a user logged in at its desktop, copy [`record.ps1`](../tests/runtime/chrome/record.ps1), [`record.html`](../tests/runtime/chrome/record.html) and `probe.js` into one directory and run, from an administrator's PowerShell or over SSH:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File record.ps1
```

It serves the page and the probe on the loopback address and starts Chrome on the page, from a new user data directory and with the flags of a Baseline's first start, `--variations-override-country=us` among them. Chrome has no variations seed yet then, so no field trial changes its features. Chrome runs windowed on the logged-in desktop, through a scheduled task, also when the script runs over SSH. The page runs the probe, as the Page program runs it, and posts what it records with Chrome's build, from `navigator.userAgentData`. The script writes it into `<build>.json` next to itself, closes Chrome and deletes the directory. Copy the file into `tests/runtime/chrome`; the tests read the one of the build in `tools/workspace/chromium_tag`.

### Tests of the bindings

| Test | What it checks | Where it runs |
| --- | --- | --- |
| The build of `//mitto:runtime` | The generator reads the Engine Release's whole database without refusing anything, and `generator_test` checks that it refuses [`native/bindings/test/unknown_extended_attribute.idl`](../native/bindings/test/unknown_extended_attribute.idl), whose extended attribute no release has, with a message that names it | CI's native job on every Host, which builds the Runtime |
| [`tests/test_page_bindings.py`](../tests/test_page_bindings.py) | What the probe records in the Page program is what Chrome of the Engine Release recorded; dispatch works; listeners outlive garbage collection; a Page without `HasUAVisualTransition` lacks `PopStateEvent.prototype.hasUAVisualTransition`; the options of the feature set | The native job on every Host, with the Page program from the installed wheel, and the Runtime's builds with AddressSanitizer and UndefinedBehaviorSanitizer |
| [`tests/test_bindings_generator.py`](../tests/test_bindings_generator.py) | The install plan's order, the names in C++, the stable set of features, the listing, refusals, and that GN's `.pydeps` name every module of the generator | Every job that runs the tests, with no build |

## Tests

| Test | What it checks | Where it runs |
| --- | --- | --- |
| [`tests/test_page_process.py`](../tests/test_page_process.py) | Through the C test program: a Page runs `1 + 1`, gets `2`, closes, and its process is gone. A killed Page fails its calls, and a request on the same Session then succeeds. Killing the process that holds the library ends the Page process. Destroying the Session ends the Page. A library with no Page program refuses to open one | CI's native job on every Host, with the Page program from the installed wheel |
| The same file | The Page process ends with its parent while a fork of the parent holds the parent's end of the channel. Fake Page programs that speak another version or an unknown message, answer a call that is not in progress, send too large a frame, refuse, or exit | The native job on macOS and Linux. Windows has no `fork()`, and the fake Page programs are scripts |
| The same file | The Page process holds no file or socket beyond its channel and what it was started with | The native job on Linux |
| [`tests/test_native.py`](../tests/test_native.py) | The C API's arguments and options, results as JSON, failures, and the time zone and locale of the options | The native job on every Host, and the Python jobs for what needs no library |
| [`tests/test_page_program.py`](../tests/test_page_program.py) | The Page program at the other end of a socket that the test holds: each message, each refusal, and the end of a script that never ends when the channel closes | The native job on macOS and Linux, and the Runtime's builds with AddressSanitizer and UndefinedBehaviorSanitizer |
| `page_messages_fuzzer` | How the library reads what a Page sends | The sanitizer workflow's fuzz job |

To run the tests of Pages against a local build, name the C test program and the Page program, which the tests copy beside the library:

```bash
export MITTO_CORE_TEST="$MITTO_WORKSPACE/chromium-<tag>/src/out/mitto/mitto_core_test"
export MITTO_PAGE_PROGRAM="$MITTO_WORKSPACE/chromium-<tag>/src/out/runtime/mitto_page"
uv run pytest tests/test_page_process.py tests/test_page_program.py
```

The tests in `tests/test_native.py` that open a Page need the Page program beside the library that `MITTO_NATIVE_LIBRARY` names, as in a wheel, and skip without it.
