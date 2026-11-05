"""Tests of the known-gaps list and of what a run reports about it. They need no native library."""

import json

import pytest

from tools.parity import Gap, KnownGapsError, dumps_known_gaps, loads_known_gaps, mismatches

GAP = Gap("157", "navigation", "h2", "tls.cipher_suites", ["A", "B"], ["A"])
OTHER_GAP = Gap("157", "fetch", "http/1.1", "request.headers.user-agent", "user-agent: Chrome", None)


def test_the_list_reads_back_as_it_was_written() -> None:
    assert loads_known_gaps(dumps_known_gaps([GAP, OTHER_GAP])) == [GAP, OTHER_GAP]


def test_an_empty_list_reads_back() -> None:
    assert dumps_known_gaps([]) == "[]\n"
    assert loads_known_gaps("[]\n") == []


def test_the_list_holds_one_gap_per_line() -> None:
    lines = dumps_known_gaps([GAP, OTHER_GAP]).splitlines()

    assert lines[0] == "["
    assert lines[-1] == "]"
    assert json.loads(lines[1].rstrip(",")) == {
        "release": "157",
        "kind": "navigation",
        "protocol": "h2",
        "field": "tls.cipher_suites",
        "baseline": ["A", "B"],
        "mitto": ["A"],
    }
    assert json.loads(lines[2])["kind"] == "fetch"


@pytest.mark.parametrize(
    "text",
    ["", "not json", "{}", "5", '["a"]', '[{"release": "157"}]', '[{"release": "157", "unknown": 1}]'],
)
def test_a_bad_list_is_refused(text: str) -> None:
    with pytest.raises(KnownGapsError):
        loads_known_gaps(text)


def test_the_same_gaps_in_any_order_are_no_mismatch() -> None:
    assert not mismatches([GAP, OTHER_GAP], [OTHER_GAP, GAP])


def test_a_gap_that_is_not_listed_is_a_mismatch() -> None:
    found = mismatches([GAP, OTHER_GAP], [GAP])

    assert found
    assert found.unlisted == [OTHER_GAP]
    assert found.gone == []


def test_a_listed_gap_that_no_longer_exists_is_a_mismatch() -> None:
    found = mismatches([GAP], [GAP, OTHER_GAP])

    assert found
    assert found.unlisted == []
    assert found.gone == [OTHER_GAP]


def test_a_gap_whose_value_changed_is_both() -> None:
    changed = Gap("157", "navigation", "h2", "tls.cipher_suites", ["A", "B"], ["B"])

    found = mismatches([changed], [GAP])

    assert found.unlisted == [changed]
    assert found.gone == [GAP]


def test_the_key_order_inside_a_value_does_not_matter() -> None:
    assert not mismatches(
        [Gap("157", "image", "h2", "tls", {"a": 1, "b": 2}, None)],
        [Gap("157", "image", "h2", "tls", {"b": 2, "a": 1}, None)],
    )


def test_a_report_names_release_kind_protocol_field_and_both_values() -> None:
    report = str(mismatches([GAP], [OTHER_GAP]))

    assert "Not in the known-gaps list (1):" in report
    assert "Chrome 157, navigation, h2, tls.cipher_suites" in report
    assert 'Baseline: ["A", "B"]' in report
    assert 'mitto:    ["A"]' in report
    assert "In the known-gaps list, but no longer there (1):" in report
    assert "Chrome 157, fetch, http/1.1, request.headers.user-agent" in report
    assert 'Baseline: "user-agent: Chrome"' in report
    assert "mitto:    (nothing)" in report
