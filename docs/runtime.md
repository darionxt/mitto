# The Runtime

The **Runtime** runs a **Page**'s scripts in a process of its own, on the **Engine Release**'s V8, while the Page's **Session** stays in the caller's process with its network, cookies and cache ([ADR-0009](adr/0009-a-process-for-each-page.md)). Terms in **bold** are defined in [CONTEXT.md](../CONTEXT.md). [building.md](building.md#build-the-runtime) says how the Page program is built. Until the release that ships the Runtime, the Page API is private, and a wheel that a release makes holds no Page program. [From Python](#from-python) says how the private API works.

## A Page and its process

Each Page is a process of the Page program, `mitto_page`, or `mitto_page.exe` on Windows. `mitto_page_open()` in the C API starts it from the directory that the library was loaded from, which is where a wheel puts both. Without a program there, as in a wheel that a release made, it fails with `ERR_FILE_NOT_FOUND`.

The library makes the channel first, then starts the program with its end of the channel and one argument, `--session=CHANNEL,PARENT`. `CHANNEL` names that end and `PARENT` is the ID of the process that holds the library. The program's standard input is the null device, and it shares standard output and error with the process that holds the library, so that what V8 or a sanitizer reports on a crash is not lost. It inherits nothing else:

| Host | The channel | What else the Page process inherits | The Page ends when the process that holds the library ends |
| --- | --- | --- | --- |
| macOS | One end of a socket pair, at descriptor 3 | Nothing: `posix_spawn()` closes every descriptor that it is not told to pass on | Its end of the channel reads the end of the stream. It also watches its parent with a `kqueue`, for when another process holds the library's end, such as one that the parent forked |
| Linux | One end of a socket pair, at descriptor 3 | Nothing: `//base` closes every other descriptor before it starts the program, and sets `PR_SET_NO_NEW_PRIVS` | The end of the stream, and the kernel's `PR_SET_PDEATHSIG`, which kills it when the thread that started it ends. So the library starts each Page from a thread that never ends |
| Windows | The client end of a named pipe, whose handle value the argument holds | Nothing: only that handle is in `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` | The pipe breaks. It also runs in a job object of its own, with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, which ends it when the last handle to the job closes, as when the process that holds the library ends |

On Windows the job also has `JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION`, so that a crash ends the process at once rather than waiting for Windows Error Reporting, and the program starts hidden, so that a caller with no console gets no window. It also starts with CET's shadow stacks off, as Chrome starts its renderers: Chromium links its programs to use them where the processor has them, and V8 swaps stacks in a way that they refuse when it leaves optimized code, which ended a script that ran a hot loop with exception `0xC0000409`. The named pipe has a random name under `\\.\pipe\LOCAL\`, one instance, and remote clients refused. Its end in the library is not inherited by any program, and the library opens the client end itself before it starts the Page, as Mojo's `PlatformChannel` does.

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

When the result is a promise, the result of the call is what the promise settles with, as when DevTools evaluates a script and waits for its promise: what it was fulfilled with, or `Uncaught (in promise) ...` and why it was rejected. The microtasks that the script queued have run by then, so a promise that they settle has settled. One that is still pending gets a handler, the call stays in progress, and the scripts after it run meanwhile. So a later script can settle it, and results come in any order. Only a promise is waited for, and not another object with a `then` method. Nothing in a Page has timers or an event loop yet, which M6's event loop brings, so a promise that only a timer would settle stays pending until the Page ends, and a caller's timeout ends the wait.

The console is V8's own, as in Chrome, where V8 installs `console` and Blink hears of each call through V8's inspector. So the program starts an inspector, as Blink does for each isolate, and the inspector hands it each message, with its arguments written as V8 writes them for Chrome's log, separated by spaces, and the level of the method that wrote it. The inspector also gives `console` what it gives in Chrome, such as `console.createTask()`. The program reports a promise rejected with no handler as Blink does, once the script and the microtasks that it queued have run, as `Uncaught (in promise) ...`, unless a handler has come since, also the one that a call adds to the promise it waits for. Errors in timers and event listeners come later, with those. A script's console messages and the errors that it leaves uncaught reach the Session before its result.

Given a script on its command line instead of `--session=`, the program runs it and prints the result, which [building.md](building.md#build-the-runtime) describes.

## The channel

The channel is the native core's own, on `//base`, and not Chromium's Mojo ([ADR-0009](adr/0009-a-process-for-each-page.md)). In the library, the network thread reads and writes it, on its message pump: with a watch on the socket on macOS and Linux, and with overlapped reads and writes on the pipe, which complete on the thread's completion port, on Windows. The Page program does the same on its I/O thread. Neither ever blocks on it: what the system cannot take yet waits in memory.

Each message travels as a frame: the size of its payload and its type, each a 32-bit little-endian number, and then the payload. Each side refuses a frame whose payload is larger than 16 MiB, so that the other cannot make it hold more for one message. So a script can be at most 16 MiB less 8 bytes, and a result whose JSON is larger fails with a message that says so.

[`native/page_messages.h`](../native/page_messages.h) holds the frames and the messages, which both programs build. [`native/page_channel.h`](../native/page_channel.h) holds the channel, with one file for macOS and Linux and one for Windows.

## The messages

Version 2 of the messages:

| Type | Name | Way | Payload |
| --- | --- | --- | --- |
| 1 | Hello | Each way, first and once | The version of the messages that the sender speaks, a 32-bit number |
| 2 | Refused | Each way | Why the sender refuses what it got, in UTF-8. The sender closes the channel after it |
| 3 | Start | To the Page, once, after the hello | A JSON object with `time_zone` and `locale`, and no other key |
| 4 | Evaluate | To the Page | The number of the call, a 64-bit number, and the script in UTF-8 |
| 5 | Evaluated | To the Session | The number of the call, a byte that is 0 for a result and 1 for a failure, and then the result as JSON, or why it failed, in UTF-8. Results come in any order, since a call whose result is a pending promise waits for it |
| 6 | Console | To the Session | A byte for the level, 1 to 5 for debug, log, info, warning and error, and then the message, in UTF-8 |
| 7 | Uncaught error | To the Session | What a console says of an error that no script caught, such as `Uncaught (in promise) Error: no`, in UTF-8 |

The library sends its hello, the start and any script at once, without waiting for the Page's hello, because the Page reads them in order. Bodies and the credit that limits them come with the Page's requests, in later versions.

Both programs come from one tree and ship together, so the version on both sides must be the same, and it changes with any change to the messages. Each side refuses, with a Refused message that says why:

- a hello of another version, and any message before the hello;
- a type that it does not know, or one that only goes the other way;
- a payload that is not what its type says, such as text that is not UTF-8 or a console message of an unknown level;
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
- `mitto_page_next_event()` takes the next event, blocking for as long as the caller says, and `mitto_page_event_free()` releases it. Beside `MITTO_PAGE_EVENT_EVALUATED` and `MITTO_PAGE_EVENT_ENDED`, a Page has a `MITTO_PAGE_EVENT_CONSOLE` event for each console message, with its level, and a `MITTO_PAGE_EVENT_UNCAUGHT_ERROR` event for each error that no script caught.
- `mitto_page_end_waits()` ends each wait of `mitto_page_next_event()` on another thread, and makes each later one return at once if no event waits, so that the thread that closes a Page can stop another that waits, as `mitto_session_end_waits()` does for a Session.
- `mitto_page_process_id()` gives the ID of the Page's process.
- `mitto_page_close()` closes the Page.

At most 1000 console messages and uncaught errors of a Page wait untaken, and at most 32 MiB of their text, so that a Page that writes without end cannot fill the memory of the process that holds the library. V8's inspector keeps as many console messages for DevTools. The library drops the ones past that, and the next one that has room comes after a console warning that says how many it dropped. Results and the end of the Page are never dropped.

A Page's events list its handle in the Session's list of what is ready, beside its requests' events. So the file descriptor that an asyncio caller watches on macOS and Linux, and `mitto_session_wait_for_event()` on Windows, wake for both. `mitto_session_next_ready()` names the handle and says whether it is a request's or a Page's.

## How a Page ends

Each way of ending reaps the process, so none is left behind:

- `mitto_page_close()` closes the channel. The Page process reads the end of the stream and ends at once. The call waits for that, for up to 2 seconds before it kills the process, and returns once the process has gone.
- `mitto_session_destroy()` does the same for each Page of the Session. Each Page gets a `MITTO_PAGE_EVENT_ENDED` event with `MITTO_PAGE_ERROR_SESSION_DESTROYED`, and its handle stays valid until it is closed.
- When the process that holds the library ends without either, the Page process ends as the table above says.
- When the Page process crashes, runs out of memory or is killed, the library reads the end of the stream on the network thread, and a worker thread reaps the process to learn how it ended. Each call in progress then fails with `MITTO_PAGE_ERROR_PROCESS_ENDED`, a `MITTO_PAGE_EVENT_ENDED` event follows, and each later call fails the same way. The message says how the process ended, such as `the Page process was ended by signal 9` or `the Page process ran out of memory`. The Session's other Pages and requests go on.

On macOS and Linux the library reaps its Page processes with `waitpid()` on the process's ID. A caller that waits for any child, or ignores `SIGCHLD`, can take a Page's exit status first. The library then says that the process ended and that something else in the process took its exit status.

## From Python

Until the release that ships the Runtime, Python's Page is private: `Session._open_page(url)` returns a Page that a `with` statement closes, and `await AsyncSession._open_page(url)` an AsyncPage that an `async with` statement closes. Neither is in `mitto.__all__` or in [api.md](api.md), and the release that ships the Runtime names them `open_page()`. On a Session whose library has no Page program beside it, as in a wheel that a release made, opening a Page raises a `NativeLibraryError` that says that the Runtime is not in this build. Starting the Page's process blocks for the few milliseconds that it takes, also on the event loop of an AsyncSession.

Until the Runtime loads documents, a Page fetches nothing: `url` is the URL that it was opened for, and its scripts run in a context that has V8's built-ins and console and nothing of a browser, no `document`, `window`, `setTimeout()` or `fetch()`. Once M6's HTML parser and fetching load a document from it, `url` becomes the document's URL.

- `evaluate(expression, timeout=None)` runs a script and returns its result as Python's `json` reads what `JSON.stringify()` writes, with `None` for what it writes nothing for. It waits for a promise to settle, as the Page program does. `timeout` is a number of seconds, as a request's total time is.
- `console_messages` holds what the scripts wrote to the console, each with its `level` and `text`, and `uncaught_errors` what a console says of each error that no script caught, for tests and debugging. Each keeps the last 1000, and fewer when their text is longer than 32 Mi characters, so that a Page that writes long messages cannot fill the caller's memory.
- `process_id` is the ID of the Page's process.
- `close()` ends the process and waits for it to go, which takes up to 2 seconds before a kill. An AsyncPage waits for that on a thread of its own.

A call fails with an error of `mitto._errors`, sorted by what a caller does next, as a request's are. Each is a `MittoError`, and private too:

| Error | What happened | What a caller does next |
| --- | --- | --- |
| `ScriptError` | The script threw, the promise that it gave was rejected, or its result cannot be read as JSON, such as a BigInt. The message is what a console says, such as `Uncaught TypeError: x is not a function` | Fix the script. The Page goes on |
| `PageEndedError` | The Page has ended: its process crashed, ran out of memory or was killed, broke the rules of the messages, or did not know its time zone or locale, or the Session closed. Each later call raises it again | Open another Page |
| `PageTimeoutError` | The timeout passed before the result arrived. It is a `TimeoutError` too | Wait longer, or close the Page: a script that never ends keeps it busy |

A Page that has been closed raises a `ValueError`, as a closed Session does. A Page waits for its events the way the Session's requests do. Page blocks in the native core with the GIL released, and several threads may use one Page: one of them at a time waits in the native core, and hands each the result that it waits for. Closing a Page while another thread waits ends that wait first, with `mitto_page_end_waits()`. An AsyncPage wakes when the Session's event descriptor, or its waiting thread on Windows, names the Page, and takes its events without waiting. A call takes at most 100 events before it looks for its result again, so that a Page that writes without end cannot keep it from its result or its timeout, or an AsyncPage hold up the loop. Closing the Session ends its Pages, whose calls then raise `PageEndedError`, and each still needs closing.

## Tests

| Test | What it checks | Where it runs |
| --- | --- | --- |
| [`tests/test_page_process.py`](../tests/test_page_process.py) | Through the C test program: a Page runs `1 + 1`, gets `2`, closes, and its process is gone. A killed Page fails its calls, and a request on the same Session then succeeds. Killing the process that holds the library ends the Page process. Destroying the Session ends the Page. A library with no Page program refuses to open one | CI's native job on every Host, with the Page program from the installed wheel |
| The same file | The Page process ends with its parent while a fork of the parent holds the parent's end of the channel. Fake Page programs that speak another version or an unknown message, answer a call that is not in progress, send too large a frame, refuse, or exit | The native job on macOS and Linux. Windows has no `fork()`, and the fake Page programs are scripts |
| The same file | The Page process holds no file or socket beyond its channel and what it was started with | The native job on Linux |
| [`tests/test_native.py`](../tests/test_native.py) | The C API's arguments and options, results as JSON, failures, the time zone and locale of the options, a promise that a later call settles, console messages and uncaught errors before the result, the end of a wait, and the limit on what waits untaken | The native job on every Host, and the Python jobs for what needs no library |
| [`tests/test_page_program.py`](../tests/test_page_program.py) | The Page program at the other end of a socket that the test holds: each message, each refusal, promises, the console and its levels, promises rejected with no handler, and the end of a script that never ends when the channel closes | The native job on macOS and Linux, and the Runtime's builds with AddressSanitizer and UndefinedBehaviorSanitizer |
| [`tests/test_page.py`](../tests/test_page.py) | Python's Page through the blocking and the asyncio API: results, promises, each error, a timeout, a killed process, closing from another thread, the Session's close, the console and uncaught errors, and a build without the Runtime | The native job on every Host, with the Page program from the installed wheel, and the Python jobs for what needs no library |
| `page_messages_fuzzer` | How the library reads what a Page sends | The sanitizer workflow's fuzz job |

To run the tests of Pages against a local build, name the C test program and the Page program, which the tests copy beside the library:

```bash
export MITTO_CORE_TEST="$MITTO_WORKSPACE/chromium-<tag>/src/out/mitto/mitto_core_test"
export MITTO_PAGE_PROGRAM="$MITTO_WORKSPACE/chromium-<tag>/src/out/runtime/mitto_page"
uv run pytest tests/test_page_process.py tests/test_page_program.py
```

The tests in `tests/test_native.py` and `tests/test_page.py` that open a Page need the Page program beside the library that `MITTO_NATIVE_LIBRARY` names, as in a wheel, and skip without it.
