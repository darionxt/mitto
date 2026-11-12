"""Writing the list of time zones that Chrome on Windows can give a page, from ICU's table of Windows' time zones.

Chrome on Windows asks ICU for the time zone, and ICU looks up the zone in Windows' settings, with the region in them, in
its windowsZones table, which comes from Unicode's CLDR, and takes the first zone that the table lists there, or the one
for the world if the region has none. V8 gives a page that zone's canonical name, and UTC for Etc/UTC and Etc/GMT. So
each first zone of the table is one that a page can read, and no other. See docs/persona.md, "What decides each value".
"""

import json
import re
from collections.abc import Iterable
from pathlib import Path

__all__ = ["TIME_ZONES_LICENCE", "TIME_ZONES_PATH", "TimeZonesError", "dumps_time_zones", "windows_time_zones"]

TIME_ZONES_PATH = Path(__file__).parents[2] / "src" / "mitto" / "time_zones.json"

TIME_ZONES_LICENCE = "Unicode-3.0"
"""The licence of ICU's data, which the list comes from."""

# Upstream: third_party/icu/source/common/wintz.cpp
# Upstream: v8/src/objects/intl-objects.cc
_UTC_NAMES = frozenset({"Etc/UTC", "Etc/GMT"})
_UTC = "UTC"

# A region's zones in the table: its code, such as US or 001, and one or more quoted strings that join into the list of its
# zones, split by spaces. The names of Windows' zones are quoted, so they are no region's code.
_REGION = re.compile(r'(?m)^\s*(\w+)\{((?:\s*"[^"]*")+)\s*\}')
_PART = re.compile(r'"([^"]*)"')


class TimeZonesError(Exception):
    """ICU's table is not what this reads."""


def windows_time_zones(table: str) -> list[str]:
    """Return the sorted names that a page can read of each zone that ICU's windowsZones table gives a region first.

    Raises:
        TimeZonesError: If the table names no zone.
    """
    found: set[str] = set()
    for region in _REGION.finditer(table):
        zones = "".join(_PART.findall(region[2])).split()
        if zones:
            found.add(_UTC if zones[0] in _UTC_NAMES else zones[0])
    if not found:
        raise TimeZonesError("the table names no time zone")
    return sorted(found)


def dumps_time_zones(source: str, time_zones: Iterable[str]) -> str:
    """Write the list as text, with one line for each zone, so that a change is one line of a diff."""
    zones = ",\n".join(f"    {json.dumps(zone)}" for zone in time_zones)
    return (
        "{\n"
        f'  "source": {json.dumps(source)},\n'
        f'  "licence": {json.dumps(TIME_ZONES_LICENCE)},\n'
        f'  "time_zones": [\n{zones}\n  ]\n'
        "}\n"
    )
