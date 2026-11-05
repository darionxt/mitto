"""Parity Checks: compare what the native core sends with what real Chrome sent. See docs/parity.md."""

from tools.parity.check import (
    BASELINES_DIRECTORY,
    Baseline,
    BaselineError,
    find_gaps,
    fingerprint,
    load_baselines,
)
from tools.parity.gaps import (
    KNOWN_GAPS_FILE_NAME,
    Gap,
    KnownGapsError,
    Mismatches,
    differences,
    dumps_known_gaps,
    loads_known_gaps,
    mismatches,
)

__all__ = [
    "BASELINES_DIRECTORY",
    "KNOWN_GAPS_FILE_NAME",
    "Baseline",
    "BaselineError",
    "Gap",
    "KnownGapsError",
    "Mismatches",
    "differences",
    "dumps_known_gaps",
    "find_gaps",
    "fingerprint",
    "load_baselines",
    "loads_known_gaps",
    "mismatches",
]
