"""The scripted steps of the release routine: uv run python -m tools.release COMMAND. See docs/releasing.md."""

import argparse
import sys
from datetime import date
from pathlib import Path

from mitto._persona import WINDOWS_PERSONAS
from tools.parity import BASELINES_DIRECTORY, BaselineError, KnownGapsError
from tools.release.chrome_releases import security_fixes
from tools.release.drop import REPOSITORY, DropError, drop_oldest
from tools.release.machine_sources import refresh_table
from tools.release.machines import STATISTICS_PATH, TABLE_PATH, MachineTableError, read_statistics, write_table
from tools.release.patch_builds import build_changes, builds_after, patch_builds_report
from tools.release.persona import PERSONAS_DIRECTORY, PersonaDataError, write_persona
from tools.release.publish import PublishError, dry_run_version, release_notes, tag_version
from tools.release.source_export import ExportError, pinned_export, read_export
from tools.release.version_history import stable_builds
from tools.release.web import WebError, fetch_from_the_network, version_parts
from tools.release.window import builds_report, next_window

ENGINE_BUILD_PATH = Path(__file__).resolve().parents[1] / "workspace" / "chromium_tag"
"""Where the repository names the build that the engine is checked out and exported from."""


def _builds(args: argparse.Namespace) -> int:
    window = next_window(WINDOWS_PERSONAS) if args.engine is None else next_window([str(args.engine - 1)])
    print(builds_report(window, date.today()))
    return 0


def _patch_builds(args: argparse.Namespace) -> int:
    since = args.since or ENGINE_BUILD_PATH.read_text(encoding="utf-8").strip()
    release = str(version_parts(since, "--since")[0])
    if args.until is not None and version_parts(args.until, "--until")[0] != int(release):
        raise WebError(f"--until {args.until} is not a build of Chrome {release}, as --since {since} is")
    export = read_export(args.archive.read_bytes()) if args.archive else pinned_export(fetch_from_the_network)
    builds = builds_after(stable_builds(release), since, args.until)
    changes = build_changes(since, builds, export.files, security_fixes(fetch_from_the_network), fetch_from_the_network)
    print(patch_builds_report(since, export.build, len(export.files), changes))
    return 0


def _persona(args: argparse.Namespace) -> int:
    path = write_persona(args.release, args.baselines, args.personas)
    print(f"Wrote {path} from the Baselines of Chrome {args.release}.")
    return 0


def _machines(args: argparse.Namespace) -> int:
    if args.saved:
        path = write_table(read_statistics(args.statistics), args.table)
        print(f"Wrote {path} from the statistics in {args.statistics}.")
        return 0
    try:
        print(refresh_table(date.today(), statistics_path=args.statistics, table_path=args.table))
    except (MachineTableError, WebError) as error:
        print(f"error: the table stays as it was, because {error}", file=sys.stderr)
        return 1
    return 0


def _drop(args: argparse.Namespace) -> int:
    dropped = drop_oldest(args.root)
    print(f"Dropped Chrome {dropped.release}: its Persona, its Baselines and {dropped.gaps} known gaps.")
    print("Wrote the line of README.md that names the Supported Window.")
    if dropped.still_named:
        print(f"These lines of code still name Chrome {dropped.release}. Change those that present it:")
        print("\n".join(f"  {named}" for named in dropped.still_named))
    print(f"Now the docs: git grep -n -w {dropped.release} -- docs README.md, and follow docs/releasing.md.")
    return 0


def _version(args: argparse.Namespace) -> int:
    print(tag_version(args.tag) if args.tag is not None else dry_run_version(args.dry_run))
    return 0


def _notes(args: argparse.Namespace) -> int:
    print(release_notes(args.version), end="")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(prog="python -m tools.release", description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)

    builds = commands.add_parser(
        "builds",
        help="find the builds of the next Supported Window and the stable date of its Engine Release",
    )
    builds.add_argument(
        "--engine",
        type=int,
        help="the Engine Release of the next Supported Window (default: the milestone after the newest Persona)",
    )
    builds.set_defaults(run=_builds)

    patch_builds = commands.add_parser(
        "patch-builds",
        help="list the Engine Release's new stable builds, and which of them change files of the source export",
    )
    patch_builds.add_argument(
        "--since", help="the build to compare with (default: the engine's, in tools/workspace/chromium_tag)"
    )
    patch_builds.add_argument("--until", help="the newest build to list (default: every newer one)")
    patch_builds.add_argument(
        "--archive", type=Path, help="a local archive of the source export, instead of downloading the pinned one"
    )
    patch_builds.set_defaults(run=_patch_builds)

    persona = commands.add_parser("persona", help="write the Persona's file of a Chrome Release from its Baselines")
    persona.add_argument("release", help="the Chrome Release, such as 158")
    persona.add_argument(
        "--baselines", type=Path, default=BASELINES_DIRECTORY, help="directory of the Baselines (default: %(default)s)"
    )
    persona.add_argument(
        "--personas", type=Path, default=PERSONAS_DIRECTORY, help="directory of the Personas (default: %(default)s)"
    )
    persona.set_defaults(run=_persona)

    machines = commands.add_parser(
        "machines", help="fetch the statistics again and write the table that a Session's Machine is drawn from"
    )
    machines.add_argument(
        "--saved", action="store_true", help="write the table from the statistics as they are, and fetch nothing"
    )
    machines.add_argument(
        "--statistics", type=Path, default=STATISTICS_PATH, help="the statistics' file (default: %(default)s)"
    )
    machines.add_argument("--table", type=Path, default=TABLE_PATH, help="the table's file (default: %(default)s)")
    machines.set_defaults(run=_machines)

    drop = commands.add_parser("drop", help="drop the oldest Chrome Release, once the new Engine Release has a Persona")
    drop.add_argument("--root", type=Path, default=REPOSITORY, help="the tree to change (default: %(default)s)")
    drop.set_defaults(run=_drop)

    version = commands.add_parser(
        "version", help="print the version that the release workflow publishes, once it is right to publish it"
    )
    source = version.add_mutually_exclusive_group(required=True)
    source.add_argument(
        "--tag", help="the tag that starts a release, which must be v and the version in pyproject.toml"
    )
    source.add_argument("--dry-run", metavar="VERSION", help="the pre-release that a dry run publishes to TestPyPI")
    version.set_defaults(run=_version)

    notes = commands.add_parser(
        "notes",
        help="print the notes of a version's GitHub release: its Supported Window, its Engine Release and its entry in "
        "CHANGELOG.md",
    )
    notes.add_argument("version", help="the version, such as 0.1.0")
    notes.set_defaults(run=_notes)

    args = parser.parse_args()
    try:
        return args.run(args)
    except (
        BaselineError,
        DropError,
        ExportError,
        KnownGapsError,
        MachineTableError,
        PersonaDataError,
        PublishError,
        WebError,
    ) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
