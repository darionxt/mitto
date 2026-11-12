"""The Parity Checks of a Script Fingerprint: run the probe in a Page as Chrome ran it, and find the gaps.

docs/parity.md, "The Script Fingerprint", says what a check does.
"""

from collections.abc import Callable, Iterable
from dataclasses import dataclass
from pathlib import Path

from mitto import Machine, Session
from mitto._native import PrivateSettings
from mitto._persona import WINDOWS_PERSONAS
from tools.capture import CaptureServer
from tools.capture.probe import RecordValue, read_record
from tools.capture.script_normal_form import script_normal_form
from tools.capture.script_page import SCRIPT_DIRECTORY, SCRIPT_RUNS, ScriptRun, posted_record
from tools.parity.check import BaselineError
from tools.parity.script_gaps import GapKind, ScriptDifference, ScriptGap, field_of, script_differences
from tools.parity.script_machine import machine_misses

__all__ = [
    "PAGE_SECONDS",
    "ScriptBaseline",
    "ScriptRecording",
    "ScriptRecordingError",
    "find_script_gaps",
    "load_script_baselines",
    "record_script",
]

PAGE_SECONDS = 120.0
"""How long a check waits for a Page to load its case page, and then for the page to post the probe's record. A Page
takes well under a second for both, and a build with a sanitizer several times as long."""

_RUNS = {run.name.removeprefix(f"{SCRIPT_DIRECTORY}/"): run for run in SCRIPT_RUNS}


class ScriptRecordingError(Exception):
    """A Page did not post a record of the probe."""


@dataclass(frozen=True, slots=True)
class ScriptBaseline:
    """What real Chrome's page saw for one Chrome Release and Recording Case of a Script Fingerprint.

    Attributes:
        release: The Chrome Release, such as "157".
        case: The Recording Case, such as "page", the name of its file.
        form: The Normal Form of the probe's record.
    """

    release: str
    case: str
    form: dict[str, RecordValue]


@dataclass(frozen=True, slots=True)
class ScriptRecording:
    """What a Page's page posted.

    Attributes:
        url: The address of the page.
        record: The probe's record, before its Normal Form.
        machine: The Machine of the Page's Session.
    """

    url: str
    record: dict[str, RecordValue]
    machine: Machine


type ScriptRecorder = Callable[[ScriptBaseline], ScriptRecording]
"""What runs the Recording Case of a Baseline in a Page, and returns what its page posted."""


def load_script_baselines(directory: Path) -> list[ScriptBaseline]:
    """Read the Baselines of a Script Fingerprint, RELEASE/script/CASE.json under directory, in a fixed order.

    Raises:
        BaselineError: A file is not a Normal Form of the probe's record, or holds a Recording Case that no run records.
    """
    baselines: list[ScriptBaseline] = []
    for path in sorted(directory.glob(f"*/{SCRIPT_DIRECTORY}/*.json")):
        if path.name.endswith(".raw.json"):
            continue
        case = path.stem
        if case not in _RUNS:
            raise BaselineError(f"{path} holds an unknown Recording Case of a Script Fingerprint: {case}")
        try:
            form = read_record(path.read_text(encoding="utf-8"))
        except ValueError as error:
            raise BaselineError(f"{path} is not a Normal Form of the probe's record: {error}") from error
        baselines.append(ScriptBaseline(path.relative_to(directory).parts[0], case, form))
    return baselines


def record_script(baseline: ScriptBaseline) -> ScriptRecording:
    """Run the Recording Case of a Baseline in a Page, on a new Session, at a new capture server.

    The Session presents the Persona of the Baseline's Chrome Release, with a Machine drawn for it as a caller's is. It
    accepts the server's private certificate and finds the case pages' names, in settings that the public API keeps
    private, as Chrome was told to. Its Page loads the case page, whose script runs the probe and posts the record, as it
    did in Chrome.

    Raises:
        BaselineError: No Persona presents the Baseline's Chrome Release.
        ScriptRecordingError: The page posted no record of the probe in time.
        RequestError: The Page could not load the case page.
        PageError: The Page ended, or did not load the case page in time.
        NativeLibraryError: The native library cannot be used.
        OSError: The Page could not start, such as with no Page program beside the library.
    """
    persona = WINDOWS_PERSONAS.get(baseline.release)
    if persona is None:
        raise BaselineError(f"no Persona presents Chrome {baseline.release}, so its Baselines cannot be checked")
    run = _RUNS[baseline.case]
    with CaptureServer() as server:
        url = server.start_url(run)
        settings = PrivateSettings(
            ignore_certificate_errors_spki_list=[server.spki_hash], host_resolver_rules=server.host_resolver_rules
        )
        with Session(persona=persona, _settings=settings) as session:
            machine = session.machine
            with session._open_page(url, timeout=PAGE_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
                if not server.wait_for_page(run.route, PAGE_SECONDS, done_path=run.done_path):
                    raise ScriptRecordingError(_no_record(run, page.console_messages, page.uncaught_errors))
        return ScriptRecording(url, _record_of(server, run), machine)


def _record_of(server: CaptureServer, run: ScriptRun) -> dict[str, RecordValue]:
    body = posted_record(server.recording(run.route, host=run.host), run)
    if body is None:
        raise ScriptRecordingError(f"{run.label}: the page posted nothing")
    try:
        return read_record(body.decode("utf-8"))
    except ValueError as error:
        raise ScriptRecordingError(f"{run.label}: the page posted no record of the probe: {error}") from error


def _no_record(run: ScriptRun, console: Iterable[object], uncaught: Iterable[str]) -> str:
    lines = [f"{run.label}: the page posted no record in {PAGE_SECONDS:g} seconds"]
    lines += [f"  uncaught: {error}" for error in uncaught]
    lines += [f"  console: {message}" for message in console]
    return "\n".join(lines)


def find_script_gaps(baselines: Iterable[ScriptBaseline], recorder: ScriptRecorder = record_script) -> list[ScriptGap]:
    """Run the Recording Case of each Baseline in a Page, and return every gap, in the order of the Baselines.

    The Page's Normal Form is compared with the Baseline's, and each value at a place that both hide as <machine> is held
    to what Chrome gives for the Page's Machine.

    Raises:
        BaselineError: No Persona presents the Chrome Release of a Baseline.
        ScriptRecordingError: A page posted no record of the probe.
        NativeLibraryError: The native library cannot be used.
    """
    gaps: list[ScriptGap] = []
    for baseline in baselines:
        recording = recorder(baseline)
        form = script_normal_form(recording.record, recording.url)
        found = [
            *script_differences(baseline.form, form),
            *(
                ScriptDifference(field_of(place), GapKind.MACHINE, chrome, page)
                for place, chrome, page in machine_misses(recording.record, recording.machine)
            ),
        ]
        gaps += [
            ScriptGap(baseline.release, baseline.case, item.kind, item.field, item.baseline, item.mitto)
            for item in found
        ]
    return gaps
