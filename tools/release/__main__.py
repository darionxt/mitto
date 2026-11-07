"""The scripted steps of the release routine: uv run python -m tools.release COMMAND. See docs/releasing.md."""

import argparse
import sys
from datetime import date
from pathlib import Path

from mitto._persona import WINDOWS_PERSONAS
from tools.parity import BASELINES_DIRECTORY, BaselineError, KnownGapsError
from tools.release.dash import DashError
from tools.release.drop import REPOSITORY, DropError, drop_oldest
from tools.release.persona import PERSONAS_DIRECTORY, PersonaDataError, write_persona
from tools.release.window import builds_report, next_window


def _builds(args: argparse.Namespace) -> int:
    window = next_window(WINDOWS_PERSONAS) if args.engine is None else next_window([str(args.engine - 1)])
    print(builds_report(window, date.today()))
    return 0


def _persona(args: argparse.Namespace) -> int:
    path = write_persona(args.release, args.baselines, args.personas)
    print(f"Wrote {path} from the Baselines of Chrome {args.release}.")
    return 0


def _drop(args: argparse.Namespace) -> int:
    dropped = drop_oldest(args.root)
    print(f"Dropped Chrome {dropped.release}: its Persona, its Baselines and {dropped.gaps} known gaps.")
    if dropped.still_named:
        print(f"These lines of code still name Chrome {dropped.release}. Change those that present it:")
        print("\n".join(f"  {named}" for named in dropped.still_named))
    print(f"Now the docs: git grep -n -w {dropped.release} -- docs README.md, and follow docs/releasing.md.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(prog="python -m tools.release", description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)

    builds = commands.add_parser(
        "builds",
        help="ask Chromium Dash for the next Supported Window's builds and the stable date of its Engine Release",
    )
    builds.add_argument(
        "--engine",
        type=int,
        help="the Engine Release of the next Supported Window (default: the milestone after the newest Persona)",
    )
    builds.set_defaults(run=_builds)

    persona = commands.add_parser("persona", help="write the Persona's file of a Chrome Release from its Baselines")
    persona.add_argument("release", help="the Chrome Release, such as 158")
    persona.add_argument(
        "--baselines", type=Path, default=BASELINES_DIRECTORY, help="directory of the Baselines (default: %(default)s)"
    )
    persona.add_argument(
        "--personas", type=Path, default=PERSONAS_DIRECTORY, help="directory of the Personas (default: %(default)s)"
    )
    persona.set_defaults(run=_persona)

    drop = commands.add_parser("drop", help="drop the oldest Chrome Release, once the new Engine Release has a Persona")
    drop.add_argument("--root", type=Path, default=REPOSITORY, help="the tree to change (default: %(default)s)")
    drop.set_defaults(run=_drop)

    args = parser.parse_args()
    try:
        return args.run(args)
    except (BaselineError, DashError, DropError, KnownGapsError, PersonaDataError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
