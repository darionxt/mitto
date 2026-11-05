"""Regenerate the known-gaps list: uv run python -m tools.parity."""

import argparse
import sys
from pathlib import Path

from mitto._native import NativeLibraryError
from tools.capture import RecordingError
from tools.parity.check import BASELINES_DIRECTORY, BaselineError, find_gaps, load_baselines
from tools.parity.gaps import KNOWN_GAPS_FILE_NAME, KnownGapsError, dumps_known_gaps, loads_known_gaps, mismatches


def main() -> int:
    parser = argparse.ArgumentParser(
        prog="python -m tools.parity",
        description="Compare the native core with every Baseline and write what differs to the known-gaps list.",
    )
    parser.add_argument(
        "--baselines",
        type=Path,
        default=BASELINES_DIRECTORY,
        help=f"directory of the Baselines and of {KNOWN_GAPS_FILE_NAME} (default: %(default)s)",
    )
    args = parser.parse_args()

    baselines: Path = args.baselines
    known_gaps = baselines / KNOWN_GAPS_FILE_NAME
    try:
        found = find_gaps(load_baselines(baselines))
        listed = loads_known_gaps(known_gaps.read_text(encoding="utf-8")) if known_gaps.exists() else []
    except (BaselineError, KnownGapsError, NativeLibraryError, RecordingError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    changes = mismatches(found, listed)
    known_gaps.write_text(dumps_known_gaps(found), encoding="utf-8")
    print(f"Wrote {len(found)} gaps to {known_gaps}: {len(changes.unlisted)} new, {len(changes.gone)} gone.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
