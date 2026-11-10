"""Compares what the Runtime's V8 compiles with to what V8 compiles with in Chrome's releases.

Chrome's releases come from official builds, whose GN args Chromium's tree keeps
for each OS in infra/config/generated/builders/ci/<os>-official/gn-args.json.
In the workspace's checkout of the Engine Release, this script asks gn what V8's
code compiles with under the Runtime's args, args.gn followed by
runtime_args.gn, and under the args of this Host's official builder: the defines
of V8, of its builtins, of cppgc and of V8's platform, and the arguments of
mksnapshot, which writes the builtins. It prints every difference but those
that DIFFERENT_ON_PURPOSE explains, and exits with 1 if there is one:

    MITTO_WORKSPACE=/absolute/path uv run python -m tools.workspace.v8_settings

It changes nothing in the checkout. See docs/releasing.md, step 2.
"""

import json
import subprocess
import sys
import tempfile
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any, cast

from tools.workspace import export

ARGS_GN = export.REPO / "tools/workspace/args.gn"
RUNTIME_ARGS_GN = export.REPO / "tools/workspace/runtime_args.gn"

OFFICIAL_ARGS = "infra/config/generated/builders/ci/{host}-official/gn-args.json"

# V8 alone is the root of the build graph, so that nothing of //mitto is needed.
V8_ROOT = ("--root-target=//v8", "--root-pattern=//v8:v8")
# The targets whose defines are compared, and the one whose arguments are.
COMPILED = ("//v8:v8_base_without_compiler", "//v8:v8_initializers", "//v8:cppgc_base", "//v8:v8_libplatform")
SNAPSHOT = "//v8:run_mksnapshot_default"

# Args of an official builder that change nothing in V8's code: how the build
# runs on Google's machines, and how much debug information it keeps.
UNCOMPARED_ARGS = frozenset(
    {"save_reproducers_on_lld_crash", "symbol_level", "use_reclient", "use_remoteexec", "use_siso"}
)

# Chrome's PGO needs profiles that the checkout leaves out, and only changes how
# the compiler lays out code it already has. For V8 it also turns on the
# profiles of V8's builtins, which bootstrap.py fetches, so that stays on.
WITHOUT_CHROMES_PGO = "chrome_pgo_phase = 0\nv8_enable_builtins_optimization = true\n"

# Defines that differ on purpose, and why.
DIFFERENT_ON_PURPOSE = {
    "V8_USE_METAGEN=0": (
        "V8 takes its instance types and layouts from Torque rather than metagen, whose gn desc of all of Chromium"
        " fails under args.gn on Linux and needs files that the source export does not hold; on macOS both gave the same"
        " Page program"
    ),
    "USE_GLIB=1": (
        "Chrome's Linux build defines it for all of Chromium with GLib, which args.gn leaves out of the library and so"
        " of the Page program; no code of V8 reads it"
    ),
    "OFFICIAL_BUILD": (
        "an official build defines it for all of Chromium, and the library's build is not official;"
        " in V8 it only decides whether the compiler keeps the names of its operators for debugging"
    ),
}


def gn_value(value: object) -> str:
    """Returns a value of a builder's JSON as GN writes it."""
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int | str):
        return json.dumps(value)
    export.die(f"an official builder's arg has the value {value!r}, which this script cannot write in GN")


def official_args(src: Path, host: export.Host) -> str:
    """Returns the GN args of this Host's official builder, without those that change nothing in V8's code."""
    path = src / OFFICIAL_ARGS.format(host=host.name)
    if not path.is_file():
        export.die(f"{path} is missing; find Chrome's official builder for {host.name} in that directory")
    args = cast(dict[str, object], json.loads(path.read_text())["gn_args"])
    if args.get("is_official_build") is not True:
        export.die(f"{path} does not set is_official_build")
    lines = [f"{name} = {gn_value(value)}" for name, value in sorted(args.items()) if name not in UNCOMPARED_ARGS]
    return "\n".join(lines) + "\n"


def describe(src: Path, host: export.Host, args: str) -> Mapping[str, Any]:
    """Returns what gn knows of V8's targets under `args`, from a build directory of its own that it removes."""
    with tempfile.TemporaryDirectory(dir=src / "out", prefix=".v8-settings-") as temporary:
        directory = Path(temporary)
        (directory / "args.gn").write_text(args)
        # gn desc needs a build.ninja but not its contents.
        (directory / "build.ninja").touch()
        # -q keeps the warning about args that V8 does not read, such as
        # mitto_enable_runtime, out of the JSON.
        command = [src / host.gn, "desc", "-q", directory, "//v8:*", "--format=json", *V8_ROOT]
        result = subprocess.run(
            command, cwd=src, env=export.build_environment(), capture_output=True, encoding="utf-8", check=False
        )
    if result.returncode != 0:
        export.die(f"gn desc failed with status {result.returncode}:\n{result.stdout}{result.stderr}")
    return cast(Mapping[str, Any], json.loads(result.stdout))


def differences(runtime: Mapping[str, Any], chrome: Mapping[str, Any]) -> list[str]:
    """Returns how V8 compiles differently under the Runtime's args, but for DIFFERENT_ON_PURPOSE."""
    found: list[str] = []
    for label in COMPILED:
        ours = set(cast(Sequence[str], runtime[label]["defines"]))
        theirs = set(cast(Sequence[str], chrome[label]["defines"]))
        found += [
            f"{label}: only the Runtime defines {name}" for name in sorted(ours - theirs - DIFFERENT_ON_PURPOSE.keys())
        ]
        found += [
            f"{label}: only Chrome defines {name}" for name in sorted(theirs - ours - DIFFERENT_ON_PURPOSE.keys())
        ]
    ours = cast(Sequence[str], runtime[SNAPSHOT]["args"])
    theirs = cast(Sequence[str], chrome[SNAPSHOT]["args"])
    if ours != theirs:
        found.append(f"{SNAPSHOT} runs with {list(ours)}, and in Chrome with {list(theirs)}")
    return found


def main(argv: Sequence[str] | None = None) -> int:
    del argv  # The script takes no arguments.
    try:
        host = export.current_host()
        src = export.workspace_dir() / f"chromium-{export.read_tag()}" / "src"
        if not (src / "v8").is_dir():
            export.die(f"{src} has no V8; run tools/workspace/bootstrap.py first")
        runtime = describe(src, host, ARGS_GN.read_text() + RUNTIME_ARGS_GN.read_text())
        chrome = describe(src, host, official_args(src, host) + WITHOUT_CHROMES_PGO)
        found = differences(runtime, chrome)
    except export.ExportError as error:
        print(f"v8_settings: error: {error}", file=sys.stderr)
        return 1
    for line in found:
        print(line)
    for name, reason in DIFFERENT_ON_PURPOSE.items():
        print(f"different on purpose: {name}: {reason}")
    if found:
        print(
            f"v8_settings: V8 compiles differently from Chrome's official {host.name} build, in {len(found)} ways",
            file=sys.stderr,
        )
        return 1
    print(f"v8_settings: V8 compiles as in Chrome's official {host.name} build")
    return 0


if __name__ == "__main__":
    sys.exit(main())
