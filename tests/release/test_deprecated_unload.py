"""Tests of what a Persona carries of the seed's kDeprecateUnload, on switches written here. They use no network."""

import pytest

from mitto._native import DeprecatedUnload, UnloadBuckets
from tools.release.deprecated_unload import deprecated_unload
from tools.release.variations_seed import FeatureSwitch, UndecidedStudy


def switch(feature: str, enabled: bool = True, params_share: float | None = None, **params: str) -> FeatureSwitch:
    return FeatureSwitch(feature, enabled, "Study", "Group", 1.0, params, params_share)


def test_unload_stays_without_the_seed() -> None:
    assert deprecated_unload([switch("Other")]).unload == DeprecatedUnload()


def test_unload_stays_when_the_seed_turns_deprecation_off_whatever_else_it_switches() -> None:
    switches = [switch("DeprecateUnload", False), switch("DeprecateUnloadByAllowList", allowlist="a.test")]

    result = deprecated_unload(switches)

    assert result.unload == DeprecatedUnload()
    assert result.switched == (
        "DeprecateUnload off: Study/Group, 100% of installations",
        "DeprecateUnloadByAllowList on: Study/Group, 100% of installations",
    )


def test_deprecation_alone_covers_every_host() -> None:
    assert deprecated_unload([switch("DeprecateUnload")]).unload == DeprecatedUnload(enabled=True)


@pytest.mark.parametrize("buckets", [None, "50"])
def test_hosts_that_change_what_a_page_sees_stay_and_go_to_review(buckets: str | None) -> None:
    # Without buckets, or with buckets that take part of the hosts, the hosts of the list are deprecated where others
    # are not.
    switches = [
        switch("DeprecateUnload", rollout_percent="50"),
        switch("DeprecateUnloadByAllowList", allowlist="a.test"),
    ]
    if buckets is not None:
        switches.append(switch("DeprecateUnloadByBucket", rollout_bucket="3"))

    result = deprecated_unload(switches)

    assert result.unload.allow_list == ("a.test",)
    others = " and a share of the others" if buckets is not None else ""
    assert result.review == (
        f"The seed deprecates unload on the 1 hosts of DeprecateUnloadByAllowList's list{others}, so the Persona holds"
        " the list: read it before it goes into the Persona's file",
    )


@pytest.mark.parametrize(
    ("allowlist", "hosts"),
    [
        # UnloadDeprecationAllowedHosts(): trimmed, and no empty host.
        (" a.test, b.test,,c.test ", ("a.test", "b.test", "c.test")),
        # An empty list covers every host.
        ("", ()),
    ],
)
def test_the_allow_list_is_its_hosts(allowlist: str, hosts: tuple[str, ...]) -> None:
    switches = [switch("DeprecateUnload"), switch("DeprecateUnloadByAllowList", allowlist=allowlist)]

    assert deprecated_unload(switches).unload == DeprecatedUnload(enabled=True, allow_list=hosts)


def test_a_full_rollout_needs_neither_a_bucket_nor_the_hosts_of_the_allow_list() -> None:
    # The seed of 2026-11-12: every host for every bucket, and a group of 1% for each bucket. The hosts of the list change
    # nothing then, so the Persona leaves them out.
    switches = [
        switch("DeprecateUnload", rollout_percent="100"),
        switch("DeprecateUnloadByAllowList", allowlist="a.test,b.test"),
        switch("DeprecateUnloadByBucket", params_share=0.01, rollout_bucket="7"),
    ]

    result = deprecated_unload(switches)

    assert result.unload == DeprecatedUnload(True, None, UnloadBuckets(100, None))
    assert result.review == ()
    assert result.unload.to_option() == {
        "enabled": True,
        "allow_list": None,
        "by_bucket": {"percent": 100, "bucket": None},
    }


def test_a_partial_rollout_with_a_bucket_of_each_installations_own_goes_to_review() -> None:
    switches = [
        switch("DeprecateUnload", rollout_percent="40"),
        switch("DeprecateUnloadByBucket", params_share=0.01, rollout_bucket="7"),
    ]

    result = deprecated_unload(switches)

    assert result.unload.by_bucket == UnloadBuckets(40, None)
    assert result.review == (
        "The seed deprecates unload on 40% of hosts with a bucket of each installation's own, so the Persona has no"
        " bucket and a Page deprecates it on no host that the allow list leaves out",
    )


@pytest.mark.parametrize(
    ("percent", "expected"), [({"rollout_percent": "25"}, 25), ({}, 100), ({"rollout_percent": "x"}, 100)]
)
def test_the_rollout_reads_its_parameters_as_chrome_does(percent: dict[str, str], expected: int) -> None:
    # base::FeatureParam<int>: its default for a missing parameter or one that is not an integer.
    switches = [
        FeatureSwitch("DeprecateUnload", True, "Study", "Group", 1.0, percent),
        switch("DeprecateUnloadByBucket", rollout_bucket="3"),
    ]

    assert deprecated_unload(switches).unload.by_bucket == UnloadBuckets(expected, 3)


def test_parameters_that_most_installations_do_not_share_go_to_review() -> None:
    switches = [
        switch("DeprecateUnload"),
        FeatureSwitch("DeprecateUnloadByAllowList", True, "Hosts", "A+B", 0.8, {"allowlist": "a.test"}, 0.4),
    ]

    result = deprecated_unload(switches)

    assert result.unload.allow_list == ("a.test",)
    assert result.review[0] == (
        "The seed turns DeprecateUnloadByAllowList on for 80% of installations in Hosts, but no parameters for more"
        " than half of them, so the Persona has those of the most"
    )


def test_a_feature_keeps_its_first_override() -> None:
    switches = [switch("DeprecateUnload", False), switch("DeprecateUnload")]

    assert deprecated_unload(switches).unload == DeprecatedUnload()


def test_a_study_that_follows_the_machine_goes_to_review_only_if_it_switches_unload() -> None:
    undecided = [
        UndecidedStudy("Buckets", ("min_os_version",), ("DeprecateUnloadByBucket",)),
        UndecidedStudy("Other", ("min_os_version",), ("Something",)),
    ]

    assert deprecated_unload([], undecided).review == (
        "The seed's study Buckets switches DeprecateUnloadByBucket by min_os_version, which follows the machine, so"
        " the Persona leaves it aside",
    )
