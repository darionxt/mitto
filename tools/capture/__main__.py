"""Record a client: uv run python -m tools.capture --out DIR."""

import argparse
import json
import sys
from pathlib import Path

from tools.capture.cases import RESUMPTION_RUNS, RUNS, Case, Run
from tools.capture.devtools import PageClicker
from tools.capture.normal_form import RecordingError, dumps_normal_form, normal_form
from tools.capture.probe import dumps_record, read_record
from tools.capture.recording import Recording, raw_form
from tools.capture.script_normal_form import dumps_script_raw_record, script_normal_form
from tools.capture.script_page import SCRIPT_RUNS, ScriptRun, posted_record
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
        choices=[run.name for run in (*SCRIPT_RUNS, *RUNS, *RESUMPTION_RUNS)],
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
    available: tuple[Run | ScriptRun, ...] = RESUMPTION_RUNS if args.tickets else (*SCRIPT_RUNS, *RUNS)
    names = args.runs or [run.name for run in available]
    runs = [run for run in available if run.name in names]
    if len(runs) != len(names):
        parser.error("--runs names a run that needs --tickets, or one that a server with --tickets cannot record")

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
    try:
        while pending:
            for run in list(pending):
                if server.wait_for_page(run.route, _POLL_SECONDS / len(pending), done_path=run.done_path):
                    failed = not _write(server, run, out) or failed
                    pending.remove(run)
    except KeyboardInterrupt:
        return 130
    return 1 if failed else 0


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


def _write(server: CaptureServer, run: Run | ScriptRun, out: Path) -> bool:
    """Write a run's recordings. Returns whether they make a complete normal form."""
    if isinstance(run, ScriptRun):
        return _write_script(server, run, out)
    recording = server.recording(run.route, host=run.host)
    raw_path = out / f"{run.name}.raw.json"
    raw_path.parent.mkdir(parents=True, exist_ok=True)
    raw_path.write_text(json.dumps(raw_form(recording), indent=2) + "\n", encoding="utf-8", newline="\n")
    written = _write_form(recording, run, run.cases, out / f"{run.name}.json", raw_path)
    if run.fresh_cases:
        written = _write_form(recording, run, run.fresh_cases, out / f"{run.name}.fresh.json", raw_path) and written
    return written


def _write_form(recording: Recording, run: Run, cases: tuple[Case, ...], path: Path, raw_path: Path) -> bool:
    """Write the normal form of some of a run's cases. Returns whether it is complete."""
    try:
        form = normal_form(recording, cases)
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


def _write_script(server: CaptureServer, run: ScriptRun, out: Path) -> bool:
    """Write the record that a run's page posted, raw and in its Normal Form. Returns whether the page posted one."""
    recording = server.recording(run.route, host=run.host)
    posted = posted_record(recording, run)
    if posted is None:
        print(f"{run.label}: the page posted no record", file=sys.stderr)
        return False
    try:
        record = read_record(posted.decode("utf-8"))
    except ValueError as error:
        print(f"{run.label}: the page posted no record of the probe: {error}", file=sys.stderr)
        return False
    url = server.start_url(run)
    peers = sorted({connection.peer for connection in recording.connections if connection.peer is not None})
    raw_path = out / f"{run.name}.raw.json"
    raw_path.parent.mkdir(parents=True, exist_ok=True)
    raw_path.write_text(dumps_script_raw_record(url, peers, record), encoding="utf-8", newline="\n")
    path = out / f"{run.name}.json"
    path.write_text(dumps_record(script_normal_form(record, url)), encoding="utf-8", newline="\n")
    print(f"{run.label}: wrote {path}", flush=True)
    return True


if __name__ == "__main__":
    sys.exit(main())
