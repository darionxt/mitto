"""The Engine Release's new stable builds, and which of them change files that the source export holds.

A patch build changes what mitto ships only where it changes a file of the export: in Chromium's own tree, or in a
submodule such as BoringSSL or QUICHE, which Chromium rolls to a new commit. Both are public as soon as the build is,
even while the bugs behind its fixes stay restricted. See docs/releasing.md.
"""

from collections import Counter
from collections.abc import Iterable
from dataclasses import dataclass, field

from tools.release.chrome_releases import SEVERITIES, SecurityFix
from tools.release.gitiles import CHROMIUM_URL, Change, Commit, commits_between, submodule_urls
from tools.release.version_history import StableBuild
from tools.release.web import Fetch, WebError, version_parts

__all__ = [
    "NOT_CODE",
    "BuildChanges",
    "ExportedChange",
    "Roll",
    "build_changes",
    "builds_after",
    "may_start_a_patch_run",
    "patch_builds_report",
]

NOT_CODE = frozenset({"chrome/VERSION", "DEPS"})
"""Files of the export that change without changing its code: the version, which every build sets, and DEPS, which
changes with every roll of a submodule, whose own changes count instead."""

_NO_COMMIT = "0" * 40


@dataclass(frozen=True, slots=True)
class ExportedChange:
    """A commit of Chromium or of one of its submodules that changes files of the export.

    Attributes:
        commit: The commit.
        submodule: The submodule's path, such as "third_party/boringssl/src", or "" for Chromium's own tree.
        files: The files of the export it changes, from the top of Chromium's tree.
        fixes: The security fixes of the release notes whose bugs it names, unless it is a revert.
        reverted: The security fixes of the release notes whose bugs it names, if it is a revert, which takes them out.
    """

    commit: Commit
    submodule: str
    files: tuple[str, ...]
    fixes: tuple[SecurityFix, ...]
    reverted: tuple[SecurityFix, ...] = ()


@dataclass(frozen=True, slots=True)
class Roll:
    """A commit of Chromium that moves a submodule which the export holds files of, and what that changes.

    Attributes:
        commit: Chromium's commit.
        submodule: The submodule's path.
        old: The submodule's commit before.
        new: The submodule's commit after.
        commits: How many commits of the submodule it rolls in.
        exported: How many of those change files of the export.
        removed: How many commits of the submodule it takes out, as a roll back to an older commit does.
        removed_exported: How many of those change files of the export.
    """

    commit: Commit
    submodule: str
    old: str
    new: str
    commits: int
    exported: int
    removed: int = 0
    removed_exported: int = 0


@dataclass(slots=True)
class _Changes:
    exported: list[ExportedChange] = field(default_factory=list[ExportedChange])
    # Commits of submodules that a roll takes out.
    removed: list[ExportedChange] = field(default_factory=list[ExportedChange])
    rolls: list[Roll] = field(default_factory=list[Roll])
    deps_alone: list[Commit] = field(default_factory=list[Commit])


@dataclass(frozen=True, slots=True)
class BuildChanges:
    """What a build changes in the export since the engine's build.

    Attributes:
        build: The build.
        gained: What it has that the engine's build has not, oldest first.
        lacks: What the engine's build has that it has not, such as on a branch made for a respin, or in a submodule
            that it rolls back.
        rolls: The rolls of submodules that the export holds files of, among what it gained.
        deps_alone: Commits it gained that change DEPS and roll no submodule, such as a new toolchain.
    """

    build: StableBuild
    gained: tuple[ExportedChange, ...]
    lacks: tuple[ExportedChange, ...]
    rolls: tuple[Roll, ...]
    deps_alone: tuple[Commit, ...]

    @property
    def carried(self) -> tuple[SecurityFix, ...]:
        """The security fixes that the build ends up with, once each, from what it gained.

        A fix that a later revert takes out is not carried, unless a reland brings it back: a fix counts when more of
        the gained commits bring it than revert it.
        """
        balance = Counter(fix.bug for change in self.gained for fix in change.fixes)
        balance.subtract(fix.bug for change in self.gained for fix in change.reverted)
        carried = {fix.bug: fix for change in self.gained for fix in change.fixes if balance[fix.bug] > 0}
        return tuple(carried.values())


def build_changes(
    since: str,
    builds: Iterable[StableBuild],
    exported: frozenset[str],
    fixes: dict[str, SecurityFix],
    fetch: Fetch,
) -> list[BuildChanges]:
    """Return what each build changes in the files of the export, since the engine's build.

    Args:
        since: The engine's build, such as "157.0.8103.27".
        builds: The builds to look at.
        exported: The files of the export, from the top of Chromium's tree.
        fixes: The security fixes of the release notes, by bug.
        fetch: What asks Chromium's Gitiles servers.

    Raises:
        WebError: A server did not answer, or its answer cannot be read.
    """
    reader = _Reader(exported, fixes, fetch)
    found: list[BuildChanges] = []
    for build in builds:
        gained = reader.changes(since, build.version)
        lacks = reader.changes(build.version, since)
        has, lacking = _net(
            [*reversed(gained.exported), *reversed(lacks.removed)],
            [*reversed(lacks.exported), *reversed(gained.removed)],
        )
        found.append(
            BuildChanges(build, has, lacking, tuple(reversed(gained.rolls)), tuple(reversed(gained.deps_alone)))
        )
    return found


def _net(
    brought: list[ExportedChange], taken: list[ExportedChange]
) -> tuple[tuple[ExportedChange, ...], tuple[ExportedChange, ...]]:
    """Return what a build has and lacks of the commits that its rolls bring in and take out, once each.

    Rolls can bring a commit of a submodule in and take it out again, such as a roll and its revert, so a commit counts
    as had when it comes in more often than it goes out, and as lacking when it goes out more often. A commit of
    Chromium's own tree comes in or goes out once at most.
    """

    def key(change: ExportedChange) -> tuple[str, str]:
        return change.submodule, change.commit.id

    balance = Counter(key(change) for change in brought)
    balance.subtract(key(change) for change in taken)
    has = {key(change): change for change in brought if balance[key(change)] > 0}
    lacks = {key(change): change for change in taken if balance[key(change)] < 0}
    return tuple(has.values()), tuple(lacks.values())


class _Reader:
    """Reads what changes between two builds in the files of the export, and asks each server once per question."""

    def __init__(self, exported: frozenset[str], fixes: dict[str, SecurityFix], fetch: Fetch) -> None:
        self._exported = exported
        self._fixes = fixes
        self._fetch = fetch
        self._commits: dict[tuple[str, str, str], list[Commit]] = {}
        self._urls: dict[str, dict[str, str]] = {}
        self._held: dict[str, bool] = {}

    def changes(self, old: str, new: str) -> _Changes:
        """Return what the commits of Chromium that new has and old has not change in the export, newest first."""
        changes = _Changes()
        for commit in self._between(CHROMIUM_URL, old, new):
            files = [
                change.path
                for change in commit.changes
                if not change.submodule
                and change.path not in NOT_CODE
                and (change.path in self._exported or change.old_path in self._exported)
            ]
            moved = [change for change in commit.changes if change.submodule and self._holds(change.path)]
            # A submodule that comes or goes changes every file of the export under it.
            files += [change.path for change in moved if _NO_COMMIT in (change.old_id, change.new_id)]
            if files:
                changes.exported.append(self._exported_change(commit, "", tuple(files)))
            for change in moved:
                if _NO_COMMIT not in (change.old_id, change.new_id):
                    self._read_roll(commit, change, new, changes)
            if "DEPS" in (change.path for change in commit.changes) and not any(
                change.submodule for change in commit.changes
            ):
                changes.deps_alone.append(commit)
        return changes

    def _read_roll(self, commit: Commit, change: Change, ref: str, changes: _Changes) -> None:
        """Add what a roll of a submodule changes in the export, and the roll, to changes.

        A roll can move a submodule forward, back to an older commit, or to a commit on another branch, so it reads
        both the commits it rolls in and those it takes out.
        """
        url = self._submodule_url(ref, change.path)
        rolled_in = self._exported_in(change.path, self._between(url, change.old_id, change.new_id))
        taken_out = self._exported_in(change.path, self._between(url, change.new_id, change.old_id))
        changes.exported += rolled_in[1]
        changes.removed += taken_out[1]
        changes.rolls.append(
            Roll(
                commit,
                change.path,
                change.old_id,
                change.new_id,
                commits=rolled_in[0],
                exported=len(rolled_in[1]),
                removed=taken_out[0],
                removed_exported=len(taken_out[1]),
            )
        )

    def _exported_in(self, submodule: str, commits: list[Commit]) -> tuple[int, list[ExportedChange]]:
        """Return how many commits of a submodule there are, and those of them that change files of the export."""
        found: list[ExportedChange] = []
        for inner in commits:
            files = tuple(
                f"{submodule}/{inner_change.path}"
                for inner_change in inner.changes
                if f"{submodule}/{inner_change.path}" in self._exported
                or f"{submodule}/{inner_change.old_path}" in self._exported
            )
            if files:
                found.append(self._exported_change(inner, submodule, files))
        return len(commits), found

    def _exported_change(self, commit: Commit, submodule: str, files: tuple[str, ...]) -> ExportedChange:
        fixes = tuple(self._fixes[bug] for bug in commit.bugs if bug in self._fixes)
        if commit.reverts:
            return ExportedChange(commit, submodule, files, fixes=(), reverted=fixes)
        return ExportedChange(commit, submodule, files, fixes)

    def _between(self, repository: str, old: str, new: str) -> list[Commit]:
        key = (repository, old, new)
        if key not in self._commits:
            self._commits[key] = commits_between(repository, old, new, self._fetch)
        return self._commits[key]

    def _holds(self, path: str) -> bool:
        """Return whether the export holds files under a path, such as a submodule's."""
        if path not in self._held:
            prefix = f"{path}/"
            self._held[path] = any(file.startswith(prefix) for file in self._exported)
        return self._held[path]

    def _submodule_url(self, ref: str, path: str) -> str:
        if ref not in self._urls:
            self._urls[ref] = submodule_urls(CHROMIUM_URL, ref, self._fetch)
        urls = self._urls[ref]
        if path not in urls:
            raise WebError(f"the .gitmodules of {ref} has no submodule at {path}")
        return urls[path]


def patch_builds_report(since: str, export_build: str, export_files: int, changes: list[BuildChanges]) -> str:
    """Return what a patch run needs to know of the Engine Release's new builds: what each changes in the export."""
    release = since.split(".", 1)[0]
    if not changes:
        return f"Google's version history has no Windows stable build of Chrome {release} after {since}."
    lines = [
        f"Since {since}, Google's version history has {_count(len(changes), 'Windows stable build')} of Chrome {release}.",
        f"Exported files are the {export_files:,} files of the source export of {export_build}.",
        "",
    ]
    rows = [("Build", "First served", "Rollout", "Changes to exported files", "Severest fix in the notes")]
    for build in changes:
        touched = _count(len(build.gained), "commit") if build.gained else "none"
        if build.lacks:
            touched += f", lacks {_count(len(build.lacks), 'commit')}"
        rows.append(
            (
                build.build.version,
                str(build.build.first_served),
                "pinnable" if build.build.pinnable else _share(build.build.rollout),
                touched,
                _severest(build.carried),
            )
        )
    widths = [max(len(row[column]) for row in rows) for column in range(len(rows[0]))]
    lines += ["  ".join(cell.ljust(width) for cell, width in zip(row, widths, strict=True)).rstrip() for row in rows]

    shown: set[str] = set()
    for build in changes:
        new = [change for change in build.gained if change.commit.id not in shown]
        rolls = [roll for roll in build.rolls if roll.commit.id not in shown]
        deps_alone = [commit for commit in build.deps_alone if commit.id not in shown]
        shown |= {change.commit.id for change in new} | {roll.commit.id for roll in rolls}
        shown |= {commit.id for commit in deps_alone}
        if not (new or rolls or deps_alone or build.lacks):
            continue
        lines += ["", f"{build.build.version}:"]
        if new:
            lines.append("  Changes exported files in these commits, new in this build:")
            lines += [line for change in new for line in _describe(change)]
        lines += [
            f"  Rolls {roll.submodule} {roll.old[:12]}..{roll.new[:12]} in {roll.commit.id[:12]}: "
            f"{_count(roll.commits, 'commit')}, of which {roll.exported} change exported files"
            + (
                f", and takes out {_count(roll.removed, 'commit')}, of which {roll.removed_exported} change them."
                if roll.removed
                else "."
            )
            for roll in rolls
        ]
        if deps_alone:
            lines.append("  Changes DEPS without a roll, which can move the toolchain that the export pins:")
            lines += [f"    {commit.id[:12]} {commit.subject}" for commit in deps_alone]
        if build.lacks:
            lines.append(f"  Lacks these commits of {since}, which change exported files:")
            lines += [line for change in build.lacks for line in _describe(change)]
    if not shown and not any(build.lacks for build in changes):
        lines += ["", "None of them changes exported files."]
    lines += ["", _verdict(changes)]
    return "\n".join(lines)


def may_start_a_patch_run(fix: SecurityFix) -> bool:
    """Return whether a fix in exported files is one for which the owner may start a patch run: one that the notes rate
    Critical, or one that they say is exploited in the wild."""
    return fix.severity == "Critical" or fix.in_the_wild


def _verdict(changes: list[BuildChanges]) -> str:
    """Say whether a build carries a fix for which the owner may start a patch run, which takes a pinnable build."""

    def candidates(build: BuildChanges) -> list[SecurityFix]:
        return [fix for fix in build.carried if may_start_a_patch_run(fix)]

    pinnable = [build for build in changes if build.build.pinnable]
    if pinnable and (fixes := candidates(pinnable[-1])):
        return (
            f"The newest pinnable build, {pinnable[-1].build.version}, carries {_count(len(fixes), 'fix')} in exported "
            "files that the notes rate Critical or say are exploited in the wild: the owner may start a patch run, "
            "as docs/releasing.md says."
        )
    if waiting := [build for build in changes if candidates(build)]:
        return (
            f"{waiting[-1].build.version} carries a fix in exported files that the notes rate Critical or say is "
            "exploited in the wild, but no pinnable build does yet, and a patch run takes a pinnable build."
        )
    return (
        "No build carries a fix in exported files that the notes rate Critical or say is exploited in the wild, so the "
        "next run takes what they change."
    )


def _severest(fixes: tuple[SecurityFix, ...]) -> str:
    if not fixes:
        return "none"
    severest = max((fix.severity for fix in fixes), key=SEVERITIES.index)
    return f"{severest}, exploited in the wild" if any(fix.in_the_wild for fix in fixes) else severest


def _describe(change: ExportedChange) -> list[str]:
    where = f"{change.submodule} " if change.submodule else ""
    lines = [f"    {where}{change.commit.id[:12]} {change.commit.subject}"]
    if change.commit.bugs:
        lines.append(f"      Bug: {', '.join(change.commit.bugs)}")
    lines += [
        f"      {fix.severity} {fix.cve}: {fix.title} (bug {fix.bug}, notes of {fix.published})"
        + (", exploited in the wild" if fix.in_the_wild else "")
        for fix in change.fixes
    ]
    lines += [
        f"      Reverts the fix of {fix.severity} {fix.cve}: {fix.title} (bug {fix.bug}, notes of {fix.published})"
        for fix in change.reverted
    ]
    lines += [f"      {file}" for file in change.files]
    return lines


def _count(number: int, noun: str) -> str:
    return f"{number} {noun}{'' if number == 1 else 's'}"


def _share(fraction: float) -> str:
    return f"up to {fraction * 100:.3g}%"


def builds_after(builds: Iterable[StableBuild], since: str, until: str | None) -> list[StableBuild]:
    """Return the builds after since, up to and with until if given, oldest first."""
    low = version_parts(since, "--since")
    high = version_parts(until, "--until") if until is not None else None
    return [
        build
        for build in builds
        if version_parts(build.version, "the version history") > low
        and (high is None or version_parts(build.version, "the version history") <= high)
    ]
