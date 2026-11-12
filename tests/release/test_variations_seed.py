"""Tests of reading a variations seed and of what its studies switch for a Persona's Chrome. They use no network: each
seed is written here, as a protocol buffer of the fields that components/variations/proto names."""

import datetime

import pytest

from tools.release.variations_seed import (
    Client,
    FeatureSwitch,
    SeedError,
    UndecidedStudy,
    fetch_seed,
    majority_switches,
    read_seed,
    seed_url,
    serial_version,
    v8_flags,
)
from tools.release.web import WebError

CLIENT = Client(version="156.0.8078.80", locale="en-US", country="us", date=datetime.date(2026, 11, 12))
WINDOWS = 0
MAC = 1
STABLE = 3
BETA = 2


def varint(value: int) -> bytes:
    if value < 0:
        value += 1 << 64
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def number(field: int, value: int) -> bytes:
    return varint(field << 3) + varint(value)


def message(field: int, content: bytes | str) -> bytes:
    data = content.encode() if isinstance(content, str) else content
    return varint(field << 3 | 2) + varint(len(data)) + data


def group(name: str, weight: int, enable: tuple[str, ...] = (), disable: tuple[str, ...] = (), **params: str) -> bytes:
    association = b"".join(message(1, feature) for feature in enable) + b"".join(
        message(2, feature) for feature in disable
    )
    data = message(1, name) + number(2, weight) + message(12, association)
    return data + b"".join(message(6, message(1, key) + message(2, value)) for key, value in params.items())


def windows_stable(*extra: bytes) -> bytes:
    """A filter for Windows on the stable channel, with more of its fields."""
    return number(4, STABLE) + number(5, WINDOWS) + b"".join(extra)


def study(name: str, *groups: bytes, filter: bytes | None = None, layer: bytes | None = None) -> bytes:
    data = message(1, name) + number(7, 1) + b"".join(message(9, item) for item in groups)
    if filter is not None:
        data += message(10, filter)
    if layer is not None:
        data += message(16, layer)
    return data


def seed(*studies: bytes, layers: tuple[bytes, ...] = ()) -> bytes:
    data = message(1, "serial") + b"".join(message(2, item) for item in studies)
    data += message(4, "20261111-170049.393000-production")
    return data + b"".join(message(6, layer) for layer in layers)


def layer(layer_id: int, slots: int, *members: tuple[int, int, int]) -> bytes:
    """A layer with members (id, first slot, last slot)."""
    data = number(1, layer_id) + number(2, slots)
    for member_id, start, end in members:
        data += message(3, number(1, member_id) + message(2, number(1, start) + number(2, end)))
    return data


def switches_of(data: bytes, client: Client = CLIENT) -> list[FeatureSwitch]:
    return majority_switches(read_seed(data), client)[0]


def test_a_study_with_one_group_for_everyone_switches_its_features() -> None:
    data = seed(study("Deprecation", group("Disabled", 100, disable=("Fledge",)), filter=windows_stable()))

    assert switches_of(data) == [
        FeatureSwitch(feature="Fledge", enabled=False, study="Deprecation", group="Disabled", share=1.0)
    ]


def test_the_seed_keeps_its_serial_number_and_version() -> None:
    read = read_seed(seed(study("A", group("G", 1), filter=windows_stable())))

    assert (read.serial_number, read.version) == ("serial", "20261111-170049.393000-production")


def test_a_study_whose_small_groups_switch_a_feature_leaves_most_installations_with_its_default() -> None:
    data = seed(
        study(
            "FencedFramesDeprecation",
            group("Disabled", 1, disable=("FencedFrames",)),
            group("Control", 1),
            group("Default", 98),
            filter=windows_stable(),
        )
    )

    assert switches_of(data) == []


def test_groups_that_switch_a_feature_alike_count_together() -> None:
    data = seed(
        study(
            "Launch",
            group("Enabled_A", 30, enable=("Thing",)),
            group("Enabled_B", 30, enable=("Thing",)),
            group("Default", 40),
            filter=windows_stable(),
        )
    )

    [switch] = switches_of(data)
    assert (switch.feature, switch.enabled, switch.group, switch.share) == ("Thing", True, "Enabled_A+Enabled_B", 0.6)


@pytest.mark.parametrize(
    "filter",
    [
        pytest.param(number(4, STABLE) + number(5, MAC), id="another-platform"),
        pytest.param(number(4, STABLE), id="no-platform"),
        pytest.param(number(4, BETA) + number(5, WINDOWS), id="another-channel"),
        pytest.param(windows_stable(message(2, "157.*")), id="newer-milestone"),
        pytest.param(windows_stable(message(3, "156.0.8061.0")), id="older-build"),
        pytest.param(windows_stable(message(10, "de")), id="another-country"),
        pytest.param(windows_stable(message(11, "us")), id="country-excluded"),
        pytest.param(windows_stable(message(6, "fr")), id="another-locale"),
        pytest.param(windows_stable(message(12, "en-US")), id="locale-excluded"),
        pytest.param(windows_stable(number(7, 1)), id="phones"),
        pytest.param(windows_stable(number(14, 0)), id="desktops-excluded"),
        pytest.param(windows_stable(number(20, 1)), id="arm"),
        pytest.param(windows_stable(number(15, 1)), id="low-end-devices"),
        pytest.param(windows_stable(number(18, 1)), id="managed-clients"),
        pytest.param(windows_stable(number(22, 42)), id="google-group"),
        pytest.param(windows_stable(message(26, "group")), id="enterprise-group"),
        pytest.param(windows_stable(number(19, 2)), id="restricted-clients-only"),
        pytest.param(windows_stable(number(1, 1_900_000_000)), id="not-started"),
        pytest.param(windows_stable(number(13, 1_700_000_000)), id="ended"),
    ],
)
def test_a_study_whose_filter_leaves_the_persona_out_switches_nothing(filter: bytes) -> None:
    data = seed(study("Study", group("Enabled", 100, enable=("Thing",)), filter=filter))

    assert switches_of(data) == []


@pytest.mark.parametrize(
    "filter",
    [
        pytest.param(windows_stable(message(2, "156.*"), message(3, "156.*")), id="its-milestone"),
        pytest.param(windows_stable(message(2, "150.0.7863.0"), message(3, "156.0.8078.80")), id="up-to-its-build"),
        pytest.param(windows_stable(message(10, "us"), message(6, "en-US")), id="its-country-and-locale"),
        pytest.param(windows_stable(message(11, "de"), message(12, "fr")), id="others-excluded"),
        pytest.param(windows_stable(number(15, 0), number(18, 0)), id="neither-low-end-nor-managed"),
        pytest.param(windows_stable(number(23, 42), number(19, 1)), id="google-group-excluded-and-critical"),
        pytest.param(windows_stable(number(1, 1_700_000_000), number(13, 1_900_000_000)), id="running"),
        pytest.param(number(4, -1) + number(4, STABLE) + number(5, WINDOWS), id="unknown-channel-too"),
    ],
)
def test_a_study_whose_filter_takes_the_persona_switches_its_features(filter: bytes) -> None:
    data = seed(study("Study", group("Enabled", 100, enable=("Thing",)), filter=filter))

    assert [switch.feature for switch in switches_of(data)] == ["Thing"]


def test_a_study_with_no_filter_applies_to_every_client() -> None:
    assert [switch.feature for switch in switches_of(seed(study("Study", group("Enabled", 1, enable=("Thing",)))))] == [
        "Thing"
    ]


def test_only_the_first_study_of_a_name_that_applies_counts() -> None:
    data = seed(
        study("UniversalOptOut", group("Enabled_Dogfood", 100, enable=("Gpc",)), filter=windows_stable(number(22, 7))),
        study("UniversalOptOut", group("Enabled", 1, enable=("Gpc",)), group("Default", 98), filter=windows_stable()),
        study("UniversalOptOut", group("Enabled", 100, enable=("Other",)), filter=windows_stable()),
    )

    assert switches_of(data) == []


@pytest.mark.parametrize(("last_slot", "switched"), [(29, False), (59, True)])
def test_a_study_in_a_layer_counts_the_share_of_slots_its_members_hold(last_slot: int, switched: bool) -> None:
    reference = number(1, 5) + number(2, 1)
    data = seed(
        study("Layered", group("Enabled", 100, enable=("Thing",)), filter=windows_stable(), layer=reference),
        layers=(layer(5, 100, (1, 0, last_slot), (2, last_slot + 1, 99)),),
    )

    assert bool(switches_of(data)) is switched


def test_a_study_in_a_layer_the_seed_does_not_have_applies_to_nobody() -> None:
    data = seed(study("Layered", group("Enabled", 100, enable=("Thing",)), layer=number(1, 9) + number(2, 1)))

    assert switches_of(data) == []


def test_a_study_whose_filter_follows_the_machine_is_left_aside_with_its_features() -> None:
    data = seed(
        study("Win24H2", group("Disabled", 100, disable=("Thing",)), filter=windows_stable(message(16, "10.0.26100")))
    )

    assert majority_switches(read_seed(data), CLIENT) == (
        [],
        [UndecidedStudy(name="Win24H2", fields=("min_os_version",), features=("Thing",))],
    )


def test_a_filter_field_that_this_does_not_know_is_an_error() -> None:
    with pytest.raises(SeedError, match="field 99"):
        read_seed(seed(study("Study", group("Enabled", 100), filter=windows_stable(number(99, 1)))))


@pytest.mark.parametrize(
    "data",
    [
        pytest.param(b"", id="empty"),
        pytest.param(message(2, b"\x0a\x05ab"), id="field-past-the-end"),
        pytest.param(b"\x0f", id="unknown-wire-type"),
        pytest.param(message(2, message(1, b"\xff")), id="not-utf-8"),
    ],
)
def test_bytes_that_are_not_a_seed_are_an_error(data: bytes) -> None:
    with pytest.raises(SeedError):
        read_seed(data)


def test_gin_turns_v8flag_features_into_flags_with_their_parameters() -> None:
    switches = [
        FeatureSwitch("V8Flag_sqrt_allocation_limits", True, "S", "G", 1.0, {"slow_factor": "80", "factor": "80"}),
        FeatureSwitch("V8Flag_maglev", False, "T", "G", 1.0),
        FeatureSwitch("V8Flag_stack_size", True, "U", "G", 1.0, {"V8FlagParam": "984", "other": "1"}),
        FeatureSwitch("NotAFlag", True, "V", "G", 1.0),
    ]

    assert v8_flags(switches) == [
        "--sqrt_allocation_limits",
        "--factor=80",
        "--slow_factor=80",
        "--no-maglev",
        "--stack_size=984",
        "--other=1",
    ]


def test_the_seed_of_a_milestone_is_asked_for_as_chrome_asks_for_it() -> None:
    asked: list[str] = []

    def fetch(url: str) -> bytes:
        asked.append(url)
        return seed(study("A", group("G", 1)))

    assert fetch_seed(157, fetch).studies[0].name == "A"
    assert asked == [seed_url(157)]
    assert seed_url(157) == (
        "https://clientservices.googleapis.com/chrome-variations/seed?osname=win&channel=stable&milestone=157"
    )


def test_an_answer_that_is_not_a_seed_is_a_web_error() -> None:
    with pytest.raises(WebError, match="did not give a variations seed"):
        fetch_seed(157, lambda url: b"<html>")


@pytest.mark.parametrize(
    ("serial_number", "version"),
    [
        pytest.param(
            "SMChYyMDI2MTEwNi0wMTAxMTAuNDkyMDAwEgsIABADGJsBIAAoARoOCggxNzkzOTg0NBICCAI=#L+RHg1zfG4I=",
            "20261106-010110.492000",
            id="chrome-txt-of-155",
        ),
        pytest.param("SM" + "ChYyMDI2MTExMS0xNzAwNDkuMzkzMDAw", "20261111-170049.393000", id="no-hash"),
        pytest.param("20261111-170049.393000#x", None, id="another-form"),
        pytest.param("SM!!!#x", None, id="not-base64"),
        pytest.param("SMEgA=#x", None, id="no-version"),
    ],
)
def test_a_serial_number_names_the_version_of_its_seed(serial_number: str, version: str | None) -> None:
    assert serial_version(serial_number) == version
