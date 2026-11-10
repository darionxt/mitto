"""Tests of which new builds of the Engine Release change files of the source export, with no network."""

import base64
import json
from datetime import date

import pytest

from tests.release.saved import gitiles_log_url, saved
from tools.release.chrome_releases import SecurityFix, security_fixes
from tools.release.gitiles import CHROMIUM_URL
from tools.release.patch_builds import build_changes, builds_after, may_start_a_patch_run, patch_builds_report
from tools.release.version_history import StableBuild, stable_builds
from tools.release.web import Fetch

# Files of the source export of 157.0.8103.39: those that the commits from 157.0.8103.27 to 157.0.8103.39 change, the
# files it holds of ANGLE, and one that nothing changes.
EXPORTED_157 = frozenset(
    {
        "DEPS",
        "chrome/VERSION",
        "net/http/http_cache_transaction.cc",
        "net/http/http_cache_writers.cc",
        "net/http/http_cache_writers.h",
        "net/http/partial_data.cc",
        "net/http/partial_data.h",
        "net/http/http_cache.cc",
        "third_party/angle/AUTHORS",
        "third_party/angle/LICENSE",
        "third_party/angle/README.chromium",
        "third_party/angle/dotfile_settings.gni",
    }
)

BORINGSSL_URL = "https://boringssl.googlesource.com/boringssl"
EXPORTED = frozenset(
    {
        "DEPS",
        "chrome/VERSION",
        "net/http/partial_data.cc",
        "net/socket/socket.cc",
        "third_party/boringssl/src/ssl/s3.cc",
    }
)


def test_157_0_8103_27_to_39_finds_the_change_to_a_206_that_overruns_its_range() -> None:
    builds = builds_after(stable_builds("157", saved), "157.0.8103.27", "157.0.8103.39")

    changes = build_changes("157.0.8103.27", builds, EXPORTED_157, security_fixes(saved), saved)

    assert [found.build.version for found in changes] == ["157.0.8103.38", "157.0.8103.39"]
    for found in changes:
        (change,) = found.gained
        assert change.commit.id.startswith("d1676d6c0e7d")
        assert change.commit.subject == "[M157] HttpCache::Transaction reject overlong range responses"
        assert change.submodule == ""
        assert change.files == (
            "net/http/http_cache_transaction.cc",
            "net/http/http_cache_writers.cc",
            "net/http/http_cache_writers.h",
            "net/http/partial_data.cc",
            "net/http/partial_data.h",
        )
        assert [(fix.severity, fix.cve, fix.title) for fix in change.fixes] == [
            ("High", "CVE-2026-76029", "Improper input validation in Network")
        ]
        assert found.lacks == ()
        assert found.deps_alone == ()
        (roll,) = found.rolls
        assert (roll.submodule, roll.commits, roll.exported) == ("third_party/angle", 1, 0)


def test_the_report_of_157_0_8103_27_to_39_shows_the_change_under_the_first_build_that_has_it() -> None:
    builds = builds_after(stable_builds("157", saved), "157.0.8103.27", "157.0.8103.39")
    changes = build_changes("157.0.8103.27", builds, EXPORTED_157, security_fixes(saved), saved)

    report = patch_builds_report("157.0.8103.27", "157.0.8103.39", len(EXPORTED_157), changes)

    assert report.splitlines()[:7] == [
        "Since 157.0.8103.27, Google's version history has 2 Windows stable builds of Chrome 157.",
        "Exported files are the 12 files of the source export of 157.0.8103.39.",
        "",
        "Build          First served  Rollout      Changes to exported files  Severest fix in the notes",
        "157.0.8103.38  2026-11-03    up to 24.8%  1 commit                   High",
        "157.0.8103.39  2026-11-03    pinnable     1 commit                   High",
        "",
    ]
    assert report.count("d1676d6c0e7d [M157] HttpCache::Transaction reject overlong range responses") == 1
    assert "      High CVE-2026-76029: Improper input validation in Network (bug 572055997, notes of 2026-10-29)" in (
        report.splitlines()
    )
    assert "157.0.8103.39:" not in report.splitlines()
    assert report.splitlines()[-1].startswith("No build carries a fix in exported files that the notes rate Critical")


def build(version: str, *, pinnable: bool = True) -> StableBuild:
    return StableBuild(version, date(2026, 11, 9), 1.0 if pinnable else 0.25, pinnable)


def commit(sha: str, message: str, *changes: dict[str, object]) -> dict[str, object]:
    return {"commit": sha, "message": message, "tree_diff": list(changes)}


def file(path: str, kind: str = "modify") -> dict[str, object]:
    return {
        "type": kind,
        "old_id": "1" * 40,
        "old_mode": 0o100644,
        "old_path": "/dev/null" if kind == "add" else path,
        "new_id": "2" * 40,
        "new_mode": 0o100644,
        "new_path": "/dev/null" if kind == "delete" else path,
    }


def submodule(path: str, old: str, new: str) -> dict[str, object]:
    return {
        "type": "modify",
        "old_id": old,
        "old_mode": 0o160000,
        "old_path": path,
        "new_id": new,
        "new_mode": 0o160000,
        "new_path": path,
    }


def answering(logs: dict[str, list[dict[str, object]]], gitmodules: str = "") -> tuple[Fetch, list[str]]:
    """Return what gives these logs by their URL, and .gitmodules, and the URLs it was asked for."""
    asked: list[str] = []

    def fetch(url: str) -> bytes:
        asked.append(url)
        if url.endswith("/.gitmodules?format=TEXT"):
            return base64.b64encode(gitmodules.encode())
        return b")]}'\n" + json.dumps({"log": logs.get(url, [])}).encode()

    return fetch, asked


def fix(bug: str, severity: str = "High", *, in_the_wild: bool = False) -> SecurityFix:
    return SecurityFix(bug, severity, f"CVE-2026-{bug}", "Use after free in Network", "2026-11-09", in_the_wild)


def test_the_version_and_deps_change_no_code_and_a_roll_counts_by_its_own_commits() -> None:
    old, new = "a" * 40, "b" * 40
    fetch, _ = answering(
        {
            gitiles_log_url(CHROMIUM_URL, "157.0.8103.39", "157.0.8103.51"): [
                commit("3" * 40, "Incrementing VERSION to 157.0.8103.51\n", file("chrome/VERSION")),
                commit(
                    "2" * 40,
                    "Roll commits from side projects in M157\n",
                    file("DEPS"),
                    submodule("third_party/boringssl/src", old, new),
                ),
                commit("1" * 40, "[M157] Fix a test\n", file("net/http/partial_data_unittest.cc")),
            ],
            gitiles_log_url(BORINGSSL_URL, old, new): [
                commit("e" * 40, "Fix the record layer\n\nBug: 580000001\n", file("ssl/s3.cc"), file("ssl/test.cc")),
                commit("d" * 40, "Update the docs\n", file("README.md")),
            ],
        },
        '[submodule "third_party/boringssl/src"]\n\tpath = third_party/boringssl/src\n'
        "\turl = https://boringssl.googlesource.com/boringssl.git\n",
    )

    (found,) = build_changes(
        "157.0.8103.39", [build("157.0.8103.51")], EXPORTED, {"580000001": fix("580000001")}, fetch
    )

    (change,) = found.gained
    assert (change.submodule, change.commit.subject, change.files) == (
        "third_party/boringssl/src",
        "Fix the record layer",
        ("third_party/boringssl/src/ssl/s3.cc",),
    )
    assert [found_fix.bug for found_fix in change.fixes] == ["580000001"]
    (roll,) = found.rolls
    assert (roll.submodule, roll.old, roll.new, roll.commits, roll.exported) == (
        "third_party/boringssl/src",
        old,
        new,
        2,
        1,
    )
    assert found.deps_alone == ()


def test_a_submodule_that_the_export_holds_nothing_of_is_never_asked() -> None:
    fetch, asked = answering(
        {
            gitiles_log_url(CHROMIUM_URL, "157.0.8103.39", "157.0.8103.51"): [
                commit("2" * 40, "Roll v8\n", file("DEPS"), submodule("v8", "a" * 40, "b" * 40)),
            ],
        }
    )

    (found,) = build_changes("157.0.8103.39", [build("157.0.8103.51")], EXPORTED, {}, fetch)

    assert (found.gained, found.rolls) == ((), ())
    assert not any("v8" in url or ".gitmodules" in url for url in asked)


def test_deps_without_a_roll_is_shown_apart() -> None:
    fetch, _ = answering(
        {
            gitiles_log_url(CHROMIUM_URL, "157.0.8103.39", "157.0.8103.51"): [
                commit("2" * 40, "[M157] Roll clang\n", file("DEPS"), file("tools/clang/scripts/update.py")),
            ],
        }
    )

    (found,) = build_changes("157.0.8103.39", [build("157.0.8103.51")], EXPORTED, {}, fetch)

    assert found.gained == ()
    assert [commit.subject for commit in found.deps_alone] == ["[M157] Roll clang"]


def test_a_build_that_lacks_a_change_of_the_engines_build_says_so() -> None:
    fetch, _ = answering(
        {
            gitiles_log_url(CHROMIUM_URL, "157.0.8103.51", "157.0.8103.39"): [
                commit("9" * 40, "[M157] Fix a socket\n\nBug: 580000002\n", file("net/socket/socket.cc")),
            ],
        }
    )

    (found,) = build_changes("157.0.8103.39", [build("157.0.8103.51")], EXPORTED, {}, fetch)

    (lacking,) = found.lacks
    assert lacking.files == ("net/socket/socket.cc",)
    report = patch_builds_report("157.0.8103.39", "157.0.8103.39", len(EXPORTED), [found])
    assert "none, lacks 1 commit" in report
    assert "  Lacks these commits of 157.0.8103.39, which change exported files:" in report.splitlines()


def test_a_renamed_or_deleted_file_of_the_export_counts() -> None:
    fetch, _ = answering(
        {
            gitiles_log_url(CHROMIUM_URL, "157.0.8103.39", "157.0.8103.51"): [
                commit("2" * 40, "Delete\n", file("net/socket/socket.cc", "delete")),
                commit("1" * 40, "Add\n", file("net/socket/new_socket.cc", "add")),
            ],
        }
    )

    (found,) = build_changes("157.0.8103.39", [build("157.0.8103.51")], EXPORTED, {}, fetch)

    assert [change.commit.subject for change in found.gained] == ["Delete"]


@pytest.mark.parametrize(
    ("severity", "in_the_wild", "expected"),
    [("Critical", False, True), ("High", True, True), ("High", False, False), ("Medium", False, False)],
)
def test_a_patch_run_may_start_for_a_critical_fix_or_one_exploited_in_the_wild(
    severity: str, in_the_wild: bool, expected: bool
) -> None:
    assert may_start_a_patch_run(fix("1", severity, in_the_wild=in_the_wild)) is expected


def socket_fix_in(*builds: StableBuild, found: SecurityFix) -> str:
    """Return the report of builds that each gain a fix to net/socket/socket.cc since 157.0.8103.39."""
    fetch, _ = answering(
        {
            gitiles_log_url(CHROMIUM_URL, "157.0.8103.39", one.version): [
                commit("5" * 40, f"[M157] Fix a socket\n\nBug: {found.bug}\n", file("net/socket/socket.cc"))
            ]
            for one in builds
        }
    )
    changes = build_changes("157.0.8103.39", builds, EXPORTED, {found.bug: found}, fetch)
    return patch_builds_report("157.0.8103.39", "157.0.8103.39", len(EXPORTED), changes)


def test_the_report_names_the_pinnable_build_that_carries_a_critical_fix() -> None:
    report = socket_fix_in(
        build("157.0.8103.50", pinnable=False), build("157.0.8103.51"), found=fix("580000007", "Critical")
    )

    assert report.splitlines()[-1].startswith(
        "The newest pinnable build, 157.0.8103.51, carries 1 fix in exported files that the notes rate Critical"
    )


def test_the_report_says_when_no_pinnable_build_carries_it_yet() -> None:
    report = socket_fix_in(build("157.0.8103.51", pinnable=False), found=fix("580000007", "High", in_the_wild=True))

    assert "High, exploited in the wild" in report
    assert report.splitlines()[-1].startswith("157.0.8103.51 carries a fix in exported files")
    assert report.splitlines()[-1].endswith("but no pinnable build does yet, and a patch run takes a pinnable build.")


def test_the_report_of_builds_that_change_nothing_says_so() -> None:
    fetch, _ = answering({})
    changes = build_changes("157.0.8103.39", [build("157.0.8103.51")], EXPORTED, {}, fetch)

    report = patch_builds_report("157.0.8103.39", "157.0.8103.39", len(EXPORTED), changes)

    assert "None of them changes exported files." in report.splitlines()


def test_the_report_of_no_new_build_says_so() -> None:
    assert patch_builds_report("157.0.8103.51", "157.0.8103.39", 1, []) == (
        "Google's version history has no Windows stable build of Chrome 157 after 157.0.8103.51."
    )


def test_the_builds_are_those_after_since_and_up_to_until() -> None:
    builds = [build(version) for version in ("157.0.8103.27", "157.0.8103.38", "157.0.8103.39", "157.0.8103.100")]

    assert [found.version for found in builds_after(builds, "157.0.8103.27", None)] == [
        "157.0.8103.38",
        "157.0.8103.39",
        "157.0.8103.100",
    ]
    assert [found.version for found in builds_after(builds, "157.0.8103.27", "157.0.8103.39")] == [
        "157.0.8103.38",
        "157.0.8103.39",
    ]


def test_a_submodule_of_the_export_that_goes_changes_its_files_and_is_never_read() -> None:
    fetch, asked = answering(
        {
            gitiles_log_url(CHROMIUM_URL, "157.0.8103.39", "157.0.8103.51"): [
                commit("2" * 40, "Drop BoringSSL\n", submodule("third_party/boringssl/src", "a" * 40, "0" * 40)),
            ],
        }
    )

    (found,) = build_changes("157.0.8103.39", [build("157.0.8103.51")], EXPORTED, {}, fetch)

    (change,) = found.gained
    assert change.files == ("third_party/boringssl/src",)
    assert found.rolls == ()
    assert not any("boringssl" in url or ".gitmodules" in url for url in asked)


def test_a_roll_back_to_an_older_commit_lacks_what_it_takes_out() -> None:
    old, new = "a" * 40, "b" * 40
    fetch, _ = answering(
        {
            gitiles_log_url(CHROMIUM_URL, "157.0.8103.39", "157.0.8103.51"): [
                commit("2" * 40, "Revert the roll of BoringSSL\n", submodule("third_party/boringssl/src", old, new)),
            ],
            gitiles_log_url(BORINGSSL_URL, new, old): [
                commit("e" * 40, "Fix the record layer\n\nBug: 580000001\n", file("ssl/s3.cc")),
                commit("d" * 40, "Update the docs\n", file("README.md")),
            ],
        },
        '[submodule "third_party/boringssl/src"]\n\tpath = third_party/boringssl/src\n'
        "\turl = https://boringssl.googlesource.com/boringssl\n",
    )

    (found,) = build_changes(
        "157.0.8103.39", [build("157.0.8103.51")], EXPORTED, {"580000001": fix("580000001")}, fetch
    )

    assert found.gained == ()
    (lacking,) = found.lacks
    assert (lacking.submodule, lacking.files) == ("third_party/boringssl/src", ("third_party/boringssl/src/ssl/s3.cc",))
    (roll,) = found.rolls
    assert (roll.commits, roll.exported, roll.removed, roll.removed_exported) == (0, 0, 2, 1)
    report = patch_builds_report("157.0.8103.39", "157.0.8103.39", len(EXPORTED), [found])
    assert "none, lacks 1 commit" in report
    assert "0 commits, of which 0 change exported files, and takes out 2 commits, of which 1 change them." in report
