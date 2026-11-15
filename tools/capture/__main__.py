"""Record a client: uv run python -m tools.capture --out DIR."""

import argparse
import json
import signal
import sys
from pathlib import Path
from types import FrameType

from tools.capture.cases import RECORDING_RUNS, RESUMPTION_RUNS, Case, Run
from tools.capture.devtools import PageClicker
from tools.capture.normal_form import RecordingError, dumps_normal_form, normal_form
from tools.capture.probe import RecordValue, dumps_record, read_record
from tools.capture.recording import Recording, raw_form
from tools.capture.script_delta import dumps_script_delta, script_delta
from tools.capture.script_normal_form import dumps_script_raw_record, script_normal_form
from tools.capture.script_page import SCRIPT_RUNS, TOP_LEVEL_PAGE, ScriptRun, posted_record
from tools.capture.server import DEFAULT_HOST, CaptureServer

_RUNS_FILE = "runs.json"
_POLL_SECONDS = 0.1


def main() -> int:
    parser = argparse.ArgumentParser(prog="python -m tools.capture", description="Record what a client sends.")
    parser.add_argument("--out", type=Path, required=True, help="directory for the recordings")
    parser.add_argument("--host", default=DEFAULT_HOST, help="a name ending in .localhost (default: %(default)s)")
    parser.add_argument("--http1-port", type=int, default=8443, help="HTTP/1.1 port (default: %(default)s)")
    parser.add_argument("--http2-port", type=int, default=8444, help="HTTP/2 port (default: %(default)s)")
    parser.add_argument("--http3-port", type=int, default=8445, help="HTTP/3 UDP port (default: %(default)s)")
    parser.add_argument("--plain-http-port", type=int, default=8446, help="plain HTTP port (default: %(default)s)")
    parser.add_argument("--proxy-port", type=int, default=8447, help="HTTP proxy port (default: %(default)s)")
    parser.add_argument(
        "--script-port", type=int, default=8448, help="port of the Script Fingerprint's pages (default: %(default)s)"
    )
    parser.add_argument(
        "--tickets",
        action="store_true",
        help="give out session tickets, and record the runs that resume instead of the others",
    )
    parser.add_argument(
        "--runs",
        nargs="+",
        choices=[run.name for run in (*SCRIPT_RUNS, *RECORDING_RUNS, *RESUMPTION_RUNS)],
        metavar="RUN",
        help="which recordings to wait for (default: all of them)",
    )
    parser.add_argument(
        "--click-in",
        type=Path,
        metavar="USER_DATA_DIRECTORY",
        help="click what the pages ask to have clicked, in the Chrome with this directory and a debugging port",
    )
    args = parser.parse_args()
    if not args.host.endswith(".localhost"):
        parser.error(f"{args.host!r} does not end in .localhost")
    available: tuple[Run | ScriptRun, ...] = RESUMPTION_RUNS if args.tickets else (*SCRIPT_RUNS, *RECORDING_RUNS)
    names = args.runs or [run.name for run in available]
    runs = [run for run in available if run.name in names]
    if len(runs) != len(names):
        parser.error("--runs names a run that needs --tickets, or one that a server with --tickets cannot record")
    if any(isinstance(run, ScriptRun) for run in runs) and TOP_LEVEL_PAGE not in runs:
        # A later case's Baseline keeps what differs from the first's, from the same session.
        parser.error(f"a run of a Script Fingerprint needs {TOP_LEVEL_PAGE.name} in the same session")

    out: Path = args.out
    out.mkdir(parents=True, exist_ok=True)
    server = CaptureServer(
        args.host,
        http1_port=args.http1_port,
        http2_port=args.http2_port,
        http3_port=args.http3_port,
        plain_http_port=args.plain_http_port,
        proxy_port=args.proxy_port,
        script_port=args.script_port,
        tickets=args.tickets,
    )
    try:
        server.start()
    except OSError as error:
        print(
            f"Cannot listen on the ports: {error}. --http1-port, --http2-port, --http3-port, --plain-http-port,"
            " --proxy-port and --script-port choose others.",
            file=sys.stderr,
        )
        return 1
    try:
        if args.click_in is None:
            return _record(server, runs, out)
        with PageClicker(args.click_in):
            return _record(server, runs, out)
    finally:
        server.close()


def _record(server: CaptureServer, runs: list[Run | ScriptRun], out: Path) -> int:
    """Say how to reach the server, and write what each run records. Returns the exit code."""
    print(f"SPKI hash: {server.spki_hash}")
    print()
    print("Start each recording from a new, empty user data directory:")
    print('  chrome --user-data-dir="NEW_EMPTY_DIRECTORY" --no-first-run --no-default-browser-check FLAGS URL')
    print()
    for run in runs:
        print(f"{run.label}: {server.start_url(run)}")
        print(f"  FLAGS: {' '.join(_quoted(flag) for flag in server.chrome_flags(run))}")
    print()
    (out / _RUNS_FILE).write_text(json.dumps(_runs_form(server, runs), indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"Waiting for the page of each to finish, or for the clicks they ask for. {_RUNS_FILE} lists them.")
    print("Ctrl-C stops early.", flush=True)
    pending = list(runs)
    failed = False
    # The first case's Normal Form, once written, which a later case's file keeps what differs from. Records come in
    # the order their pages post them, so a later case waits for it.
    first: dict[str, RecordValue] | None = None
    ctrl_c = _CtrlC()
    previous_handler = signal.signal(signal.SIGINT, ctrl_c.note)
    try:
        while pending and not ctrl_c.came:
            for run in list(pending):
                if _waits_for_the_first_case(run, pending):
                    continue
                if not server.wait_for_page(run.route, _POLL_SECONDS / len(pending), done_path=run.done_path):
                    continue
                pending.remove(run)
                if isinstance(run, ScriptRun):
                    form = _write_script(server, run, out, first)
                    first = form if run == TOP_LEVEL_PAGE else first
                    failed = form is None or failed
                else:
                    failed = not _write(server, run, out) or failed
    finally:
        signal.signal(signal.SIGINT, previous_handler)
    if ctrl_c.came:
        _say_what_waited(server, pending)
        return 130
    return 1 if failed else 0


class _CtrlC:
    """Notes that Ctrl-C came, for the wait for the pages to stop at its next short wait.

    A KeyboardInterrupt would come wherever the main thread is, which can be inside threading.Event.wait() after it let
    go of its lock and before it took it back. Leaving the wait then raises RuntimeError instead, and the command line
    exits 1.
    """

    def __init__(self) -> None:
        self.came = False

    def note(self, signal_number: int, frame: FrameType | None) -> None:
        """Handle SIGINT."""
        self.came = True


def _waits_for_the_first_case(run: Run | ScriptRun, pending: list[Run | ScriptRun]) -> bool:
    """Whether a run is a later Recording Case of a Script Fingerprint whose first case has not been written yet."""
    return isinstance(run, ScriptRun) and run != TOP_LEVEL_PAGE and TOP_LEVEL_PAGE in pending


def _say_what_waited(server: CaptureServer, pending: list[Run | ScriptRun]) -> None:
    """Name each later case whose record came but was not written, because the first case's never did."""
    for run in pending:
        if _waits_for_the_first_case(run, pending) and server.wait_for_page(run.route, 0, done_path=run.done_path):
            print(
                f"{run.label}: its record came, but nothing was written for it, since {TOP_LEVEL_PAGE.label} never"
                " posted one to keep what differs from",
                file=sys.stderr,
            )


def _quoted(flag: str) -> str:
    name, separator, value = flag.partition("=")
    return f'{name}{separator}"{value}"' if " " in value else flag


def _runs_form(server: CaptureServer, runs: list[Run | ScriptRun]) -> dict[str, list[dict[str, str | list[str]]]]:
    """Return what a script needs to take a recording session through each run.

    Each run's "fingerprint" says what it records: "wire", or "script" for a Script Fingerprint, whose start of Chrome
    must not have a debugging port of number 0, which makes navigator.webdriver true.
    """
    return {
        "runs": [
            {
                "name": run.name,
                "label": run.label,
                "fingerprint": "script" if isinstance(run, ScriptRun) else "wire",
                "url": server.start_url(run),
                "flags": server.chrome_flags(run),
            }
            for run in runs
        ]
    }


def _write(server: CaptureServer, run: Run, out: Path) -> bool:
    """Write a run's recordings. Returns whether they make a complete normal form."""
    recording = server.recording(run.route, host=run.host)
    raw_path = out / f"{run.name}.raw.json"
    raw_path.parent.mkdir(parents=True, exist_ok=True)
    raw_path.write_text(json.dumps(raw_form(recording), indent=2) + "\n", encoding="utf-8", newline="\n")
    written = _write_form(recording, run, run.cases, run.ordered, out / f"{run.name}.json", raw_path)
    if run.fresh_cases:
        written = _write_form(recording, run, run.fresh_cases, (), out / f"{run.name}.fresh.json", raw_path) and written
    return written


def _write_form(
    recording: Recording, run: Run, cases: tuple[Case, ...], ordered: tuple[Case, ...], path: Path, raw_path: Path
) -> bool:
    """Write the normal form of some of a run's cases. Returns whether it is complete."""
    try:
        form = normal_form(recording, cases, ordered)
    except RecordingError as error:
        print(f"{run.label}: {error}. See {raw_path}.", file=sys.stderr)
        return False
    path.write_text(dumps_normal_form(form), encoding="utf-8", newline="\n")
    requests = form["requests"]
    assert isinstance(requests, dict)
    missing = [case.name for case in cases if case.name not in requests]
    if missing:
        print(f"{run.label}: nothing for {', '.join(missing)}. See {raw_path}.", file=sys.stderr)
        return False
    print(f"{run.label}: wrote {path}", flush=True)
    return True


def _write_script(
    server: CaptureServer, run: ScriptRun, out: Path, first: dict[str, RecordValue] | None
) -> dict[str, RecordValue] | None:
    """Write the record that a run's page posted, raw and in its Normal Form, or for a later Recording Case what of its
    Normal Form differs from the first case's.

    Args:
        server: The server whose route has the record.
        run: The run.
        out: The output directory.
        first: The first case's Normal Form, written earlier in this session, or None if its page posted no record.

    Returns:
        The run's Normal Form, or None if nothing was written for it.
    """
    recording = server.recording(run.route, host=run.host)
    posted = posted_record(recording, run)
    if posted is None:
        print(f"{run.label}: the page posted no record", file=sys.stderr)
        return None
    try:
        record = read_record(posted.decode("utf-8"))
    except ValueError as error:
        print(f"{run.label}: the page posted no record of the probe: {error}", file=sys.stderr)
        return None
    url = server.start_url(run)
    peers = sorted({connection.peer for connection in recording.connections if connection.peer is not None})
    raw_path = out / f"{run.name}.raw.json"
    raw_path.parent.mkdir(parents=True, exist_ok=True)
    raw_path.write_text(dumps_script_raw_record(url, peers, record), encoding="utf-8", newline="\n")
    path = out / f"{run.name}.json"
    form = script_normal_form(record, url, run.machine_places)
    if run == TOP_LEVEL_PAGE:
        path.write_text(dumps_record(form), encoding="utf-8", newline="\n")
        print(f"{run.label}: wrote {path}", flush=True)
        return form
    if first is None:
        print(
            f"{run.label}: wrote only {raw_path}, since {TOP_LEVEL_PAGE.label} has no Normal Form to keep what"
            " differs from",
            file=sys.stderr,
        )
        return None
    path.write_text(dumps_script_delta(script_delta(first, form, TOP_LEVEL_PAGE.case)), encoding="utf-8", newline="\n")
    print(f"{run.label}: wrote what differs from {TOP_LEVEL_PAGE.name}.json to {path}", flush=True)
    return form


if __name__ == "__main__":
    sys.exit(main())
