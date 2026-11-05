"""Tests of how two normal forms are compared. They need no native library."""

from typing import Any

from hypothesis import given, settings
from hypothesis import strategies as st

from tests.parity.conftest import sample_form
from tools.capture.recording import Json
from tools.parity import differences


def diff(baseline: Any, mitto: Any) -> list[tuple[str, Json, Json]]:
    return list(differences(baseline, mitto))


def extension(form: dict[str, Any], name: str) -> dict[str, Any]:
    return next(item for item in form["tls"]["extensions"] if item["type"] == name)


def test_a_normal_form_has_no_differences_from_itself() -> None:
    assert diff(sample_form("http2"), sample_form("http2")) == []
    assert diff(sample_form("http1"), sample_form("http1")) == []


def test_a_value_differs_in_its_own_field() -> None:
    mitto = sample_form("http2")
    mitto["tls"]["legacy_version"] = "TLS 1.3"

    assert diff(sample_form("http2"), mitto) == [("tls.legacy_version", "TLS 1.2", "TLS 1.3")]


def test_a_field_on_one_side_only_differs_from_nothing() -> None:
    assert diff({"a": {"b": 1}}, {"a": {}}) == [("a.b", 1, None)]
    assert diff({"a": {}}, {"a": {"b": 1}}) == [("a.b", None, 1)]


def test_a_list_of_plain_values_differs_as_a_whole() -> None:
    baseline = sample_form("http2")
    mitto = sample_form("http2")
    mitto["tls"]["cipher_suites"].pop()

    assert diff(baseline, mitto) == [
        ("tls.cipher_suites", baseline["tls"]["cipher_suites"], mitto["tls"]["cipher_suites"])
    ]


def test_a_missing_extension_is_one_difference() -> None:
    baseline = sample_form("http2")
    mitto = sample_form("http2")
    mitto["tls"]["extensions"].remove(extension(mitto, "trust_anchors"))

    assert diff(baseline, mitto) == [("tls.extensions.trust_anchors", extension(baseline, "trust_anchors"), None)]


def test_an_extra_extension_is_one_difference() -> None:
    baseline = sample_form("http2")
    mitto = sample_form("http2")
    mitto["tls"]["extensions"].append({"type": "0x1234", "data": "00"})

    assert diff(baseline, mitto) == [("tls.extensions.0x1234", None, {"type": "0x1234", "data": "00"})]


def test_a_difference_inside_an_extension_is_in_the_extensions_field() -> None:
    mitto = sample_form("http2")
    extension(mitto, "supported_groups")["groups"].pop()

    assert diff(sample_form("http2"), mitto) == [
        (
            "tls.extensions.supported_groups.groups",
            ["GREASE", "X25519MLKEM768", "x25519", "secp256r1", "secp384r1"],
            ["GREASE", "X25519MLKEM768", "x25519", "secp256r1"],
        )
    ]


def test_items_of_the_same_type_compare_in_turn() -> None:
    baseline = {"extensions": [{"type": "GREASE", "length": 0}, {"type": "GREASE", "length": 1}]}
    mitto = {"extensions": [{"type": "GREASE", "length": 0}, {"type": "GREASE", "length": 2}]}

    assert diff(baseline, mitto) == [("extensions.GREASE#2.length", 1, 2)]


def test_headers_compare_by_name() -> None:
    baseline = {"headers": ["host: a", "accept: b", "user-agent: c"]}
    mitto = {"headers": ["host: a", "user-agent: d", "x-extra: e"]}

    assert diff(baseline, mitto) == [
        ("headers.accept", "accept: b", None),
        ("headers.user-agent", "user-agent: c", "user-agent: d"),
        ("headers.x-extra", None, "x-extra: e"),
    ]


def test_the_name_of_a_header_keeps_its_case() -> None:
    assert diff({"headers": ["Accept: a"]}, {"headers": ["accept: a"]}) == [
        ("headers.Accept", "Accept: a", None),
        ("headers.accept", None, "accept: a"),
    ]


def test_pseudo_headers_and_empty_values_have_names() -> None:
    assert diff({"headers": [":path: /", "user-agent: "]}, {"headers": [":path: /a", "user-agent: b"]}) == [
        ("headers.:path", ":path: /", ":path: /a"),
        ("headers.user-agent", "user-agent: ", "user-agent: b"),
    ]


def test_repeated_headers_compare_in_turn() -> None:
    assert diff({"headers": ["cookie: a", "cookie: b"]}, {"headers": ["cookie: a"]}) == [
        ("headers.cookie#2", "cookie: b", None)
    ]


def test_headers_in_another_order_are_one_difference() -> None:
    assert diff({"headers": ["a: 1", "b: 2", "c: 3"]}, {"headers": ["a: 1", "c: 3", "b: 2"]}) == [
        ("headers.<order>", ["a", "b", "c"], ["a", "c", "b"])
    ]


def test_the_order_looks_only_at_headers_on_both_sides() -> None:
    assert diff({"headers": ["a: 1", "b: 2", "c: 3"]}, {"headers": ["a: 1", "c: 3"]}) == [("headers.b", "b: 2", None)]
    assert diff({"headers": ["a: 1", "b: 2", "c: 3"]}, {"headers": ["c: 3", "a: 1"]}) == [
        ("headers.b", "b: 2", None),
        ("headers.<order>", ["a", "c"], ["c", "a"]),
    ]


def test_settings_compare_by_name() -> None:
    mitto = sample_form("http2")
    mitto["http2"]["preface"][0]["settings"][2] = "INITIAL_WINDOW_SIZE: 1"

    assert diff(sample_form("http2"), mitto) == [
        (
            "http2.preface.SETTINGS.settings.INITIAL_WINDOW_SIZE",
            "INITIAL_WINDOW_SIZE: 6291456",
            "INITIAL_WINDOW_SIZE: 1",
        )
    ]


def test_a_grease_setting_has_an_identity() -> None:
    assert diff({"settings": ["A: 1"]}, {"settings": ["A: 1", "GREASE"]}) == [("settings.GREASE", None, "GREASE")]


def test_frames_in_another_order_are_one_difference() -> None:
    baseline = sample_form("http2")
    mitto = sample_form("http2")
    mitto["http2"]["preface"].reverse()

    assert diff(baseline, mitto) == [
        ("http2.preface.<order>", ["SETTINGS", "WINDOW_UPDATE"], ["WINDOW_UPDATE", "SETTINGS"])
    ]


def test_fields_the_comparison_has_never_heard_of_compare_like_all_others() -> None:
    baseline = {"quic": {"transport_parameters": [{"type": "max_idle_timeout", "value": 30000}], "version": 1}}
    mitto = {"quic": {"transport_parameters": [{"type": "max_idle_timeout", "value": 1}], "version": 2}}

    assert diff(baseline, mitto) == [
        ("quic.transport_parameters.max_idle_timeout.value", 30000, 1),
        ("quic.version", 1, 2),
    ]


def test_values_of_different_shapes_differ_as_a_whole() -> None:
    assert diff({"a": [1]}, {"a": {"b": 1}}) == [("a", [1], {"b": 1})]
    assert diff({"a": ["x: 1"]}, {"a": ["y"]}) == [("a", ["x: 1"], ["y"])]
    assert diff({"a": 1}, {"a": None}) == [("a", 1, None)]


_FORMS = st.recursive(
    st.none()
    | st.booleans()
    | st.integers(0, 2)
    | st.sampled_from(["GREASE", "plain", "a: 1", "a: 2", "b: 1", "a#2: 1"]),
    lambda children: (
        st.lists(children, max_size=4) | st.dictionaries(st.sampled_from(["type", "x", "y"]), children, max_size=3)
    ),
    max_leaves=12,
)


@settings(derandomize=True, deadline=None)
@given(_FORMS, _FORMS)
def test_there_is_a_difference_exactly_when_two_forms_are_not_equal(baseline: Json, mitto: Json) -> None:
    assert bool(diff(baseline, mitto)) == (baseline != mitto)


@settings(derandomize=True, deadline=None)
@given(
    st.lists(st.sampled_from(["a: 1", "a: 2", "b: 1", "c: 1", "GREASE"]), max_size=5).flatmap(
        lambda items: st.tuples(st.just(items), st.permutations(items))
    )
)
def test_a_reordered_list_is_found(lists: tuple[list[str], list[str]]) -> None:
    baseline, mitto = lists
    assert bool(diff(list[Json](baseline), list[Json](mitto))) == (baseline != mitto)
