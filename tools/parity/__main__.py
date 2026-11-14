"""Regenerate the known-gaps list: uv run python -m tools.parity, or with --script that of the Script Fingerprint."""

import argparse
import sys
from collections import Counter
from collections.abc import Iterable, Iterator
from pathlib import Path

from mitto import MittoError
from mitto._native import NativeLibraryError
from tools.capture import RecordingError
from tools.parity.check import BASELINES_DIRECTORY, Baseline, BaselineError, find_gaps, load_baselines
from tools.parity.gaps import KNOWN_GAPS_FILE_NAME, Gap, KnownGapsError, dumps_known_gaps, loads_known_gaps, mismatches
from tools.parity.replay import made_by_a_page
from tools.parity.script import ScriptBaseline, ScriptRecordingError, find_script_gaps, load_script_baselines
from tools.parity.script_gaps import SCRIPT_KNOWN_GAPS_FILE_NAME, GapKind, ScriptGap, loads_script_known_gaps


def _gaps_by_case(baselines: Iterable[Baseline], gaps: Iterable[Gap]) -> Iterator[str]:
    """Yield one line for each Recording Case in the Baselines, so that one with no gaps shows that it matches, and say
    which a Page has to make."""
    counts = Counter(gap.case for gap in gaps)
    for case in dict.fromkeys(baseline.case for baseline in baselines):
        issue = made_by_a_page(case)
        yield f"  {case}: {counts[case]}" if issue is None else f"  {case}: skipped until a Page makes it (#{issue})"


def _script_counts(baselines: Iterable[ScriptBaseline], gaps: Iterable[ScriptGap]) -> Iterator[str]:
    """Yield one line for each kind of gap, and one for each Chrome Release in the Baselines, so that a count of 0 shows
    too."""
    kinds = Counter(gap.kind for gap in gaps)
    releases = Counter(gap.release for gap in gaps)
    yield "Gaps by kind, over every Chrome Release:"
    yield from (f"  {kind}: {kinds[kind]}" for kind in GapKind)
    yield "Gaps by Chrome Release:"
    yield from (
        f"  {release}: {releases[release]}" for release in sorted({b.release for b in baselines} | releases.keys())
    )


def _write_script_list(baselines: Path) -> int:
    known_gaps = baselines / SCRIPT_KNOWN_GAPS_FILE_NAME
    try:
        listed = loads_script_known_gaps(known_gaps.read_text(encoding="utf-8")) if known_gaps.exists() else []
        loaded = load_script_baselines(baselines)
        found = find_script_gaps(loaded)
    except (BaselineError, KnownGapsError, NativeLibraryError, ScriptRecordingError, MittoError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    changes = mismatches(found, listed)
    known_gaps.write_text(dumps_known_gaps(found), encoding="utf-8")
    print(f"Wrote {len(found)} gaps to {known_gaps}: {len(changes.unlisted)} new, {len(changes.gone)} gone.")
    print("\n".join(_script_counts(loaded, found)))
    skipped = list(dict.fromkeys(baseline.case for baseline in loaded if not baseline.in_a_page))
    if skipped:
        print(f"Skipped, since a Page has no frames and no workers yet: {', '.join(skipped)}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        prog="python -m tools.parity",
        description="Compare the native core with every Baseline and write what differs to the known-gaps list.",
    )
    parser.add_argument(
        "--baselines",
        type=Path,
        default=BASELINES_DIRECTORY,
        help="directory of the Baselines and of the known-gaps lists (default: %(default)s)",
    )
    parser.add_argument(
        "--script",
        action="store_true",
        help=f"compare a Page with the Baselines of the Script Fingerprint, and write {SCRIPT_KNOWN_GAPS_FILE_NAME}",
    )
    args = parser.parse_args()

    baselines: Path = args.baselines
    if args.script:
        return _write_script_list(baselines)
    known_gaps = baselines / KNOWN_GAPS_FILE_NAME
    try:
        loaded = load_baselines(baselines)
        found = find_gaps(loaded)
        listed = loads_known_gaps(known_gaps.read_text(encoding="utf-8")) if known_gaps.exists() else []
    except (BaselineError, KnownGapsError, NativeLibraryError, RecordingError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    changes = mismatches(found, listed)
    known_gaps.write_text(dumps_known_gaps(found), encoding="utf-8")
    print(f"Wrote {len(found)} gaps to {known_gaps}: {len(changes.unlisted)} new, {len(changes.gone)} gone.")
    if loaded:
        print("Gaps by Recording Case, over every Chrome Release and protocol:")
        print("\n".join(_gaps_by_case(loaded, found)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
