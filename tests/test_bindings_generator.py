"""Tests of the Runtime's bindings generator, in native/bindings, on definitions written here.

These need neither Blink's scripts nor a build. The build runs the generator over the Engine Release's whole Web IDL
database, and native/bindings/test/generator_test.py checks there that it refuses an unknown extended attribute with a
message that names it; tests/test_page_bindings.py checks what the bindings it writes do in the Page program.
"""

import dataclasses
from pathlib import Path

import pytest
from bindings_generator import extended_attributes
from bindings_generator.cpp_names import file_name, function_name, member_name
from bindings_generator.cpp_types import Site, TypeMapper
from bindings_generator.cpp_writer import ImplementedInterface, condition_test, reflected_attribute
from bindings_generator.errors import UnhandledError
from bindings_generator.install_plan import Phase, Property, plan_interface
from bindings_generator.listing import write_listing
from bindings_generator.model import (
    ALWAYS,
    Attribute,
    Condition,
    Constant,
    ExtendedAttributes,
    IdlType,
    Interface,
    Operation,
    OperationGroup,
    TypeKind,
)
from bindings_generator.runtime_features import read_features

BINDINGS = Path(__file__).resolve().parents[1] / "native" / "bindings"

BOOLEAN = IdlType(TypeKind.BOOLEAN, "boolean")
UNDEFINED = IdlType(TypeKind.UNDEFINED, "undefined")


def _extended(*keys: str) -> ExtendedAttributes:
    return ExtendedAttributes(tuple((key, ()) for key in keys))


def _attribute(name: str, *keys: str, condition: Condition = ALWAYS, readonly: bool = True) -> Attribute:
    return Attribute(name, BOOLEAN, readonly, False, _extended(*keys), condition)


def _operation(name: str, *keys: str, condition: Condition = ALWAYS, length: int = 0) -> OperationGroup:
    operation = Operation(name, (), UNDEFINED, False, _extended(*keys), condition)
    return OperationGroup(name, (operation,), False, length, _extended(*keys), condition)


def _interface(**members: object) -> Interface:
    interface = Interface(
        kind="interface",
        name="Thing",
        parent=None,
        tag=1,
        max_subclass_tag=1,
        exposure=(("Window", None),),
        condition=ALWAYS,
        extended_attributes=ExtendedAttributes(),
        attributes=(),
        constants=(),
        constructor_group=None,
        legacy_factory_functions=(),
        operation_groups=(),
        stringifier=None,
        collection=None,
        special_operations=(),
        legacy_window_aliases=(),
        exposed_constructs=(),
        unscopables=(),
    )
    return dataclasses.replace(interface, **members)  # pyright: ignore[reportArgumentType]


def _names(plan: list[Property]) -> list[str]:
    return [f"{property_.target}.{property_.name}" for property_ in plan]


def test_a_feature_that_chromes_content_layer_turns_on_is_stable() -> None:
    # content/child/runtime_features.cc copies kLocalNetworkAccessChecks, on by default, into it, which the file of
    # features does not say.
    features = read_features([{"name": "LocalNetworkAccessPermissionPolicy", "base_feature": "none"}])

    assert [feature.stable for feature in features] == [True]


@pytest.mark.parametrize(
    ("name", "file", "function", "member"),
    [
        ("PopStateEvent", "pop_state_event", "PopStateEvent", "pop_state_event"),
        ("DOMException", "dom_exception", "DOMException", "dom_exception"),
        ("hasUAVisualTransition", "has_ua_visual_transition", "HasUAVisualTransition", "has_ua_visual_transition"),
        ("composedPath", "composed_path", "ComposedPath", "composed_path"),
        # Blink's tokens, which its patterns alone would split.
        ("HTMLOListElement", "html_olist_element", "HTMLOListElement", "html_olist_element"),
        ("HTMLBRElement", "html_br_element", "HTMLBRElement", "html_br_element"),
        ("innerHTML", "inner_html", "InnerHTML", "inner_html"),
    ],
)
def test_it_names_cpp_after_web_idl(name: str, file: str, function: str, member: str) -> None:
    assert (file_name(name), function_name(name), member_name(name)) == (file, function, member)


def test_it_refuses_an_extended_attribute_it_does_not_know_and_names_it() -> None:
    with pytest.raises(UnhandledError, match=r"^attribute Thing\.on: .* \[Unheard\]$"):
        extended_attributes.check("attribute Thing.on", ["Measure", "Unheard"])


def test_it_knows_every_extended_attribute_with_a_reason() -> None:
    assert all(reason for _, reason in extended_attributes.KNOWN.values())


def test_the_plan_puts_attributes_then_constants_then_operations() -> None:
    interface = _interface(
        attributes=(_attribute("b"), _attribute("a")),
        constants=(Constant("ONE", IdlType(TypeKind.INTEGER, "unsigned short"), 1, ExtendedAttributes(), ALWAYS),),
        operation_groups=(_operation("run"),),
    )

    assert _names(plan_interface(interface)) == [
        "prototype.@@toStringTag",
        "prototype.b",
        "prototype.a",
        "prototype.ONE",
        "interface.ONE",
        "prototype.run",
        "prototype.constructor",
    ]


def test_members_behind_a_feature_come_after_the_others_on_the_templates() -> None:
    feature = Condition(features=("Feature",))
    interface = _interface(
        attributes=(_attribute("gated", condition=feature), _attribute("always")),
        operation_groups=(_operation("later", condition=feature), _operation("now")),
    )

    plan = plan_interface(interface)

    assert _names(plan) == [
        "prototype.@@toStringTag",
        "prototype.always",
        "prototype.now",
        "prototype.gated",
        "prototype.later",
        "prototype.constructor",
    ]
    assert {property_.phase for property_ in plan} == {Phase.TEMPLATE}


def test_members_that_each_context_decides_come_after_the_constructor() -> None:
    secure = Condition(other=("SecureContext",), context_dependent=True)
    interface = _interface(attributes=(_attribute("secure", condition=secure), _attribute("always")))

    plan = plan_interface(interface)

    assert _names(plan)[-2:] == ["prototype.constructor", "prototype.secure"]
    assert plan[-1].phase is Phase.CONTEXT


def test_unforgeable_members_go_on_the_instance_and_cannot_be_configured() -> None:
    plan = plan_interface(_interface(attributes=(_attribute("isTrusted", "LegacyUnforgeable"),)))

    trusted = next(property_ for property_ in plan if property_.name == "isTrusted")
    assert (trusted.target, trusted.configurable, trusted.enumerable, trusted.setter) == ("instance", False, True, None)


def test_fast_calls_come_after_the_other_members_of_their_kind() -> None:
    interface = _interface(operation_groups=(_operation("fast", "NoAllocDirectCall"), _operation("slow")))

    assert _names(plan_interface(interface))[1:3] == ["prototype.slow", "prototype.fast"]


def test_writable_and_replaceable_attributes_have_setters() -> None:
    interface = _interface(attributes=(_attribute("open", readonly=False), _attribute("swap", "Replaceable")))

    setters = {property_.name: property_.setter for property_ in plan_interface(interface)}
    assert (setters["open"], setters["swap"]) == ("set open", "set swap")


def test_the_listing_names_each_member_with_what_it_is_behind() -> None:
    interface = _interface(
        attributes=(_attribute("gated", condition=Condition(features=("Feature",))),),
        constructor_group=_operation("constructor", length=1),
    )

    assert write_listing([interface]).splitlines() == [
        "interface Thing, exposed on Window",
        "  Thing.length = 1",
        "  Thing.prototype.@@toStringTag: value, not enumerable",
        "  Thing.prototype.gated: get [RuntimeEnabled=Feature]",
        "  Thing.prototype.constructor: value",
    ]


def test_stable_features_are_those_stable_on_windows_or_on_by_their_base_feature() -> None:
    features = read_features(
        [
            {"name": "Everywhere", "status": "stable"},
            {"name": "WindowsToo", "status": {"Android": "stable", "default": "stable", "Win": "stable"}},
            {"name": "NotWindows", "status": {"Android": "stable", "default": "experimental"}},
            {"name": "ByBaseFeature", "status": "experimental", "base_feature_status": "enabled"},
            {"name": "OnlyIfOverridden", "base_feature_status": "enabled", "copied_from_base_feature_if": "overridden"},
            {"name": "NoBaseFeature", "base_feature": "none", "base_feature_status": "enabled"},
            {"name": "Test", "status": "test", "depends_on": ["Everywhere"], "implied_by": ["NotWindows"]},
        ]
    )

    assert {feature.name: feature.stable for feature in features} == {
        "Everywhere": True,
        "WindowsToo": True,
        "NotWindows": False,
        "ByBaseFeature": True,
        "OnlyIfOverridden": False,
        "NoBaseFeature": False,
        "Test": False,
    }
    assert (features[-1].depends_on, features[-1].implied_by) == (("Everywhere",), ("NotWindows",))


@pytest.mark.parametrize(
    ("entry", "message"),
    [
        pytest.param({"name": "A", "status": "stable", "unheard": 1}, "does not handle the key unheard", id="key"),
        pytest.param({"name": "A", "depends_on": ["Missing"]}, "names Missing, which is not a feature", id="name"),
    ],
)
def test_it_refuses_a_feature_it_cannot_read(entry: dict[str, object], message: str) -> None:
    with pytest.raises(UnhandledError, match=message):
        read_features([entry])


def test_the_cpp_writer_refuses_a_type_it_cannot_convert_and_names_it() -> None:
    types = TypeMapper(frozenset({"Event"}), frozenset())

    with pytest.raises(UnhandledError, match=r"^argument x of Thing\.f: .* of type long long$"):
        types.argument_type(IdlType(TypeKind.INTEGER, "long long"), "argument x of Thing.f")
    with pytest.raises(UnhandledError, match=r"of type Observable$"):
        types.return_type(IdlType(TypeKind.INTERFACE, "Observable"), "operation EventTarget.when return type")


@pytest.mark.parametrize("pydeps", ["generate_bindings.pydeps", "test/generator_test.pydeps"])
def test_the_pydeps_name_every_module_of_the_generator(pydeps: str) -> None:
    # GN rebuilds the bindings when a file that a .pydeps names changes.
    path = BINDINGS / pydeps
    lines = [line for line in path.read_text(encoding="utf-8").splitlines() if not line.startswith("#")]
    named = {(path.parent / line).resolve() for line in lines if not line.startswith("../../")}
    modules = {module.resolve() for module in (BINDINGS / "bindings_generator").glob("*.py")}

    assert modules | {(BINDINGS / "generate_bindings.py").resolve()} <= named


def test_interfaces_from_the_context_snapshot_install_their_featured_members_last() -> None:
    # Chrome's renderer takes Document's objects from its V8 context snapshot, and installs what is behind a feature
    # on them after what the context decides.
    in_the_context = Condition(other=("SecureContext",), context_dependent=True)
    members = {
        "attributes": (
            _attribute("plain"),
            _attribute("featured", condition=Condition(features=("F",))),
            _attribute("secure", condition=in_the_context),
        )
    }
    plan = plan_interface(_interface(name="Document", **members))
    other = plan_interface(_interface(**members))

    assert _names(plan)[1:] == [
        "prototype.plain",
        "prototype.constructor",
        "prototype.secure",
        "prototype.featured",
    ]
    assert _names(other)[1:] == ["prototype.plain", "prototype.featured", "prototype.constructor", "prototype.secure"]
    assert next(p for p in plan if p.name == "featured").phase is Phase.CONTEXT


def test_a_union_loses_the_interfaces_that_have_no_class() -> None:
    types = TypeMapper(frozenset({"Node"}), frozenset())
    trusted = IdlType(TypeKind.INTERFACE, "TrustedScript")
    string = IdlType(TypeKind.STRING, "DOMString")
    node = IdlType(TypeKind.INTERFACE, "Node")

    assert types.reduce(IdlType(TypeKind.UNION, "x", nullable=True, members=(string, trusted))) == dataclasses.replace(
        string, nullable=True
    )
    assert types.reduce(IdlType(TypeKind.UNION, "x", members=(node, string, trusted))).members == (node, string)
    assert not types.can_hold_a_value(IdlType(TypeKind.UNION, "x", members=(trusted,)))
    assert types.argument_type(IdlType(TypeKind.UNION, "x", members=(node, string, trusted)), "a") == (
        "const NodeOrDOMString&"
    )


@pytest.mark.parametrize(
    ("keys", "type_name", "expected"),
    [
        ((("Reflect", ()),), "DOMString", "accesskey"),
        ((("Reflect", ("http_equiv",)),), "DOMString", "http-equiv"),
        ((("Reflect", ()),), "boolean", "accesskey"),
        ((("Reflect", ()), ("URL", ())), "DOMString", None),
        ((("Reflect", ()), ("ReflectOnly", ("a",))), "DOMString", None),
        ((), "DOMString", None),
    ],
)
def test_the_writer_reflects_strings_and_booleans_itself(
    keys: tuple[tuple[str, tuple[str, ...]], ...], type_name: str, expected: str | None
) -> None:
    kind = TypeKind.BOOLEAN if type_name == "boolean" else TypeKind.STRING
    attribute = Attribute("accessKey", IdlType(kind, type_name), False, False, ExtendedAttributes(keys), ALWAYS)

    assert reflected_attribute(attribute) == expected


@pytest.mark.parametrize(
    ("condition", "test"),
    [
        (ALWAYS, "true"),
        (Condition(features=("F",)), "features.IsEnabled(RuntimeFeature::kF)"),
        (Condition(other=("SecureContext",)), "data->secure_context()"),
        (Condition(other=("RuntimeEnabled=G in the context",)), "features.IsEnabled(RuntimeFeature::kG)"),
        (Condition(other=("Exposed=Window,Worker",)), "true"),
        (Condition(other=("Exposed=Worker",)), None),
        (Condition(other=("ContextEnabled=X",)), None),
        (Condition(other=("CrossOriginIsolated",)), None),
    ],
)
def test_a_page_installs_what_its_conditions_allow(condition: Condition, test: str | None) -> None:
    # A Page is a secure top-level window with no origin trials and nothing a context enables.
    assert condition_test(condition, "Thing.x") == test


def test_an_interface_names_what_its_class_has() -> None:
    implemented = ImplementedInterface("Thing", "mitto/dom/thing.h", implemented=("a", "get b"))
    missing = ImplementedInterface("Thing", "mitto/dom/thing.h", missing=("c",))

    assert [implemented.has_behaviour("a"), implemented.has_behaviour("b", "get")] == [True, True]
    assert [implemented.has_behaviour("b"), implemented.has_behaviour("c")] == [False, False]
    assert [missing.has_behaviour("a"), missing.has_behaviour("c")] == [True, False]


def test_touch_event_detection_starts_as_on_a_desktop_without_a_touch_screen() -> None:
    [feature] = read_features([{"name": "TouchEventFeatureDetection", "status": "stable"}])

    assert not feature.stable


def test_the_cpp_types_of_the_constructs_that_timers_and_fetch_use() -> None:
    types = TypeMapper(frozenset({"Request"}), frozenset(), {"RequestMode": ("navigate", "cors")})
    byte_string = IdlType(TypeKind.STRING, "ByteString")
    pair = IdlType(TypeKind.SEQUENCE, "sequence<ByteString>", members=(byte_string,))
    pairs = IdlType(TypeKind.SEQUENCE, "sequence<sequence<ByteString>>", members=(pair,))
    record = IdlType(TypeKind.RECORD, "record<ByteString, ByteString>", members=(byte_string, byte_string))
    headers_init = IdlType(TypeKind.UNION, "x", members=(pairs, record))

    assert types.argument_type(byte_string, "a") == "const std::string&"
    assert types.argument_type(IdlType(TypeKind.CALLBACK_FUNCTION, "Function"), "a") == "v8::Local<v8::Function>"
    assert types.argument_type(IdlType(TypeKind.ENUMERATION, "RequestMode"), "a") == "const std::u16string&"
    assert types.argument_type(pairs, "a") == "const std::vector<std::vector<std::string>>&"
    assert types.argument_type(record, "a") == "const std::vector<std::pair<std::string, std::string>>&"
    assert types.argument_type(headers_init, "a") == "const ByteStringSequenceSequenceOrByteStringByteStringRecord&"
    assert types.return_type(IdlType(TypeKind.PROMISE, "Promise<any>"), "a") == "v8::Local<v8::Promise>"
    assert types.return_type(dataclasses.replace(byte_string, nullable=True), "a") == "std::optional<std::string>"
    assert types.default_value(IdlType(TypeKind.INTEGER, "long"), "0", "a") == "0"
    assert types.default_value(byte_string, '""', "a") == '""'


def test_the_cpp_types_of_what_navigator_returns() -> None:
    types = TypeMapper(frozenset(), frozenset())
    string = IdlType(TypeKind.STRING, "DOMString")
    languages = IdlType(TypeKind.FROZEN_ARRAY, "FrozenArray<DOMString>", members=(string,))
    hardware_concurrency = IdlType(TypeKind.INTEGER, "unsigned long long")

    # The class makes a frozen array or an object, and returns it as it is.
    assert types.return_type(languages, "a") == "v8::Local<v8::Value>"
    assert types.to_v8(languages, "impl->Languages()") == "impl->Languages()"
    assert types.return_type(IdlType(TypeKind.OBJECT, "object"), "a") == "v8::Local<v8::Object>"
    # A 64-bit integer goes to a page as a Number, which a V8 Integer of 32 bits could not hold.
    assert types.return_type(hardware_concurrency, "a") == "uint64_t"
    assert types.to_v8(hardware_concurrency, "x") == "v8::Number::New(isolate, static_cast<double>(x))"
    assert types.to_v8(IdlType(TypeKind.INTEGER, "unsigned long"), "x") == "v8::Integer::NewFromUnsigned(isolate, x)"


def test_an_enumeration_converts_only_to_its_strings() -> None:
    types = TypeMapper(frozenset(), frozenset(), {"RequestMode": ("navigate", "cors")})
    lines = types.from_v8(IdlType(TypeKind.ENUMERATION, "RequestMode"), "value", "mode", Site(fail="return;"), "a")

    assert 'u"navigate", u"cors"' in lines[0]
    assert "FromV8Enumeration(isolate, value, " in lines[2]
    assert '"RequestMode"' in lines[2]
    with pytest.raises(UnhandledError, match="the enumeration Other is not in the database"):
        types.argument_type(IdlType(TypeKind.ENUMERATION, "Other"), "a")


def test_a_callback_function_argument_throws_chromes_message_for_a_value_that_is_not_callable() -> None:
    types = TypeMapper(frozenset(), frozenset())
    lines = types.from_v8(
        IdlType(TypeKind.CALLBACK_FUNCTION, "VoidFunction"),
        "info[0]",
        "callback",
        Site(fail="return;", argument_index=0),
        "a",
    )

    assert 'ExceptionMessages::ArgumentNotOfType(0, "Function")' in "\n".join(lines)
