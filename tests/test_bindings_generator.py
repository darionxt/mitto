"""Tests of the Runtime's bindings generator, in native/bindings, on definitions written here.

These need neither Blink's scripts nor a build. The build runs the generator over the Engine Release's whole Web IDL
database, and native/bindings/test/generator_test.py checks there that it refuses an unknown extended attribute with a
message that names it; tests/test_page_bindings.py checks what the bindings it writes do in the Page program.
"""

import dataclasses
from pathlib import Path

import pytest
from bindings_generator import cpp_writer, extended_attributes
from bindings_generator.cpp_names import file_name, function_name, member_name
from bindings_generator.cpp_types import Site, TypeMapper
from bindings_generator.cpp_writer import (
    ImplementedInterface,
    Keywords,
    ReflectionKind,
    checks_setter_arguments,
    condition_test,
    reflected_attribute,
    reflected_keywords,
    reflection_kind,
    write_bindings,
)
from bindings_generator.errors import UnhandledError
from bindings_generator.install_plan import Phase, Property, plan_interface
from bindings_generator.known_gaps import KnownGaps, default_tojson_attributes, has_default_tojson
from bindings_generator.listing import write_listing
from bindings_generator.model import (
    ALWAYS,
    Argument,
    Attribute,
    CollectionLike,
    Condition,
    Constant,
    Dictionary,
    DictionaryMember,
    ExposedConstruct,
    ExtendedAttributes,
    IdlType,
    Interface,
    Operation,
    OperationGroup,
    SpecialOperation,
    TypeKind,
)
from bindings_generator.runtime_features import read_features
from bindings_generator.table_writer import Construct, KnownGapContext, write_tables
from bindings_generator.type_tables import TypeTables
from bindings_generator.web_idl_reader import Definitions

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


@pytest.mark.parametrize("name", ["LocalNetworkAccessPermissionPolicy", "PaymentApp", "PaymentRequest"])
def test_a_feature_that_chromes_content_layer_turns_on_is_stable(name: str) -> None:
    # Chrome's content layer copies a base::Feature of another name, on by default, into each, which the file of
    # features does not say.
    features = read_features([{"name": name, "status": "experimental", "base_feature": "none"}])

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

    with pytest.raises(UnhandledError, match=r"^argument x of Thing\.f: .* of type boolean\?$"):
        types.argument_type(IdlType(TypeKind.BOOLEAN, "boolean", nullable=True), "argument x of Thing.f")
    with pytest.raises(UnhandledError, match=r"of type boolean\?$"):
        types.return_type(IdlType(TypeKind.BOOLEAN, "boolean", nullable=True), "attribute Thing.on")


def test_64_bit_and_nullable_numbers_convert_with_null_for_null_and_undefined() -> None:
    # CookieInit's expires and maxAge.
    types = TypeMapper(frozenset({"Event"}), frozenset())
    nullable_double = IdlType(TypeKind.FLOATING_POINT, "double", nullable=True)
    nullable_long_long = IdlType(TypeKind.INTEGER, "long long", nullable=True)

    assert types.argument_type(IdlType(TypeKind.INTEGER, "long long"), "x") == "int64_t"
    assert types.argument_type(nullable_double, "x") == "std::optional<double>"
    assert types.argument_type(nullable_long_long, "x") == "std::optional<int64_t>"
    assert types.default_value(nullable_double, "null", "x") == "std::nullopt"
    converted = "\n".join(types.from_v8(nullable_long_long, "info[0]", "value", Site(fail="return;"), "x"))
    assert "if (!info[0]->IsNullOrUndefined()) {" in converted
    assert "FromV8Integer(isolate, info[0], &value_value)" in converted


def test_a_class_can_give_null_for_an_interface_without_a_class_whatever_the_idl_says() -> None:
    # Blink's classes give null for a type that the IDL does not make nullable, as Document's rootElement does.
    types = TypeMapper(frozenset({"Event"}), frozenset())

    assert types.return_type(IdlType(TypeKind.INTERFACE, "SVGSVGElement"), "x") == "bindings::ScriptWrappable*"
    assert types.return_type(IdlType(TypeKind.INTERFACE, "Event"), "x") == "mitto::Event*"


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


def test_a_union_converts_a_number_to_its_floating_point_member() -> None:
    types = TypeMapper(frozenset(), frozenset({"Options"}))
    string = IdlType(TypeKind.STRING, "DOMString")
    number = IdlType(TypeKind.FLOATING_POINT, "double")
    union = IdlType(TypeKind.UNION, "x", members=(string, number))

    assert types.argument_type(union, "a") == "const DOMStringOrDouble&"
    assert types.union_alternative(number) == "double"
    header, source = cpp_writer._write_union("DOMStringOrDouble", union, types)  # pyright: ignore[reportPrivateUsage]
    text = "\n".join(source)

    assert "using DOMStringOrDouble = std::variant<std::u16string, double>;" in header
    # A Number converts as the member's own type, with its check of finite values, before anything is a string.
    assert text.index("if (value->IsNumber()) {") < text.index("FromV8String(isolate, value, &string)")
    assert 'FromV8Double(isolate, value, true, "double", &number_double)' in text
    unrestricted = IdlType(TypeKind.UNION, "y", members=(IdlType(TypeKind.FLOATING_POINT, "unrestricted double"),))
    assert types.argument_type(dataclasses.replace(unrestricted, members=(*unrestricted.members, string)), "b") == (
        "const UnrestrictedDoubleOrDOMString&"
    )


def test_a_union_with_a_dictionary_takes_an_empty_dictionary_for_its_default() -> None:
    types = TypeMapper(frozenset(), frozenset({"Options"}))
    union = IdlType(
        TypeKind.UNION, "x", members=(IdlType(TypeKind.STRING, "DOMString"), IdlType(TypeKind.DICTIONARY, "Options"))
    )

    assert types.default_value(union, "{}", "a") == "DOMStringOrOptions(Options())"
    with pytest.raises(UnhandledError, match="default value"):
        types.default_value(dataclasses.replace(union, members=union.members[:1] * 2), "{}", "b")


def test_a_buffer_source_in_a_dictionary_converts_as_blinks_union_of_a_buffer_and_a_view() -> None:
    # WebAuthn's challenge: BufferSource, which a dictionary member holds as the union it is.
    types = TypeMapper(frozenset(), frozenset())
    buffer = IdlType(TypeKind.BUFFER_SOURCE, "ArrayBuffer")
    view = IdlType(TypeKind.BUFFER_SOURCE, "ArrayBufferView")
    union = IdlType(TypeKind.UNION, "(ArrayBuffer or ArrayBufferView)", members=(buffer, view))

    assert types.value_type(union, "a") == "ArrayBufferOrArrayBufferView"
    header, source = cpp_writer._write_union("ArrayBufferOrArrayBufferView", union, types)  # pyright: ignore[reportPrivateUsage]
    text = "\n".join(source)

    assert (
        "using ArrayBufferOrArrayBufferView = std::variant<v8::Local<v8::ArrayBuffer>, v8::Local<v8::ArrayBufferView>>;"
        in header
    )
    # bind_gen's union.py hands a SharedArrayBuffer to the ArrayBuffer member, whose conversion refuses it, and tries the
    # view next; anything else is not of the union.
    assert text.index("value->IsArrayBuffer() || value->IsSharedArrayBuffer()") < text.index(
        "value->IsArrayBufferView()"
    )
    assert "FromV8ArrayBufferMember(value, exception_state, &buffer_source)" in text
    assert "FromV8ArrayBufferViewMember(value, exception_state, &buffer_source)" in text
    assert 'ExceptionMessages::ValueNotOfType("(ArrayBuffer or ArrayBufferView)")' in text
    # Only BufferSource's members: a typed array of its own is not one the union takes.
    with pytest.raises(UnhandledError, match="a union"):
        types.value_type(dataclasses.replace(union, members=(IdlType(TypeKind.BUFFER_SOURCE, "Uint8Array"), view)), "b")


def test_a_union_with_a_boolean_takes_a_boolean_default() -> None:
    # Document.importNode()'s optional (boolean or ImportNodeOptions) options = false.
    union = IdlType(
        TypeKind.UNION,
        "boolean or ImportNodeOptions",
        members=(BOOLEAN, IdlType(TypeKind.DICTIONARY, "ImportNodeOptions")),
    )
    types = TypeMapper(frozenset(), frozenset({"ImportNodeOptions"}), {})

    assert types.default_value(union, "false", "a") == "BooleanOrImportNodeOptions(false)"


def test_a_sequence_takes_an_empty_list_for_its_default() -> None:
    # OTPCredentialRequestOptions' transports.
    types = TypeMapper(frozenset(), frozenset(), {"OTPCredentialTransportType": ("sms",)})
    transports = IdlType(
        TypeKind.SEQUENCE,
        "sequence<OTPCredentialTransportType>",
        members=(IdlType(TypeKind.ENUMERATION, "OTPCredentialTransportType"),),
    )

    assert types.default_value(transports, "[]", "a") == "{}"
    with pytest.raises(UnhandledError, match="default value"):
        types.default_value(dataclasses.replace(transports, nullable=True), "[]", "b")


_TYPES_BY_NAME = {
    "DOMString": IdlType(TypeKind.STRING, "DOMString"),
    "DOMString?": IdlType(TypeKind.STRING, "DOMString", nullable=True),
    "USVString": IdlType(TypeKind.STRING, "USVString"),
    "boolean": BOOLEAN,
    "long": IdlType(TypeKind.INTEGER, "long"),
    "unsigned long": IdlType(TypeKind.INTEGER, "unsigned long"),
    "unsigned short": IdlType(TypeKind.INTEGER, "unsigned short"),
    "double": IdlType(TypeKind.FLOATING_POINT, "double"),
    "Element?": IdlType(TypeKind.INTERFACE, "Element", nullable=True),
    "[LegacyNullToEmptyString] DOMString": IdlType(
        TypeKind.STRING, "DOMString", extended_attributes=_extended("LegacyNullToEmptyString")
    ),
    "[Clamp] unsigned long": IdlType(TypeKind.INTEGER, "unsigned long", extended_attributes=_extended("Clamp")),
}


@pytest.mark.parametrize(
    ("keys", "type_name", "expected", "kind"),
    [
        ((("Reflect", ()),), "DOMString", "accesskey", ReflectionKind.STRING),
        ((("Reflect", ("http_equiv",)),), "DOMString", "http-equiv", ReflectionKind.STRING),
        ((("Reflect", ()),), "USVString", "accesskey", ReflectionKind.STRING),
        ((("Reflect", ()),), "[LegacyNullToEmptyString] DOMString", "accesskey", ReflectionKind.STRING),
        ((("Reflect", ("aria_label",)),), "DOMString?", "aria-label", ReflectionKind.NULLABLE_STRING),
        ((("Reflect", ()),), "boolean", "accesskey", ReflectionKind.BOOLEAN),
        ((("Reflect", ()),), "long", "accesskey", ReflectionKind.INTEGER),
        ((("Reflect", ()),), "unsigned long", "accesskey", ReflectionKind.UNSIGNED_INTEGER),
        ((("Reflect", ()), ("URL", ())), "DOMString", "accesskey", ReflectionKind.URL),
        ((("Reflect", ()), ("URL", ())), "USVString", "accesskey", ReflectionKind.URL),
        ((("Reflect", ()), ("ReflectOnly", ("a",))), "DOMString", "accesskey", ReflectionKind.KEYWORDS),
        ((("Reflect", ()), ("ReflectOnly", ("a",))), "DOMString?", "accesskey", ReflectionKind.NULLABLE_KEYWORDS),
        ((("Reflect", ()), ("ReflectMissing", ("a",))), "DOMString", None, None),
        ((("Reflect", ()), ("ReflectOnly", ("a",))), "boolean", None, None),
        ((("Reflect", ()), ("URL", ())), "DOMString?", None, None),
        ((("Reflect", ()),), "unsigned short", None, None),
        ((("Reflect", ()),), "double", None, None),
        ((("Reflect", ()),), "Element?", None, None),
        ((("Reflect", ()),), "[Clamp] unsigned long", None, None),
        ((), "DOMString", None, None),
    ],
)
def test_the_writer_reflects_strings_booleans_numbers_and_urls_itself(
    keys: tuple[tuple[str, tuple[str, ...]], ...], type_name: str, expected: str | None, kind: ReflectionKind | None
) -> None:
    # bind_gen's _make_reflect_accessor_func_name(): an Element and other types stay with the class.
    idl_type = _TYPES_BY_NAME[type_name]
    attribute = Attribute("accessKey", idl_type, False, False, ExtendedAttributes(keys), ALWAYS)

    assert (reflected_attribute(attribute), reflection_kind(attribute)) == (expected, kind)


@pytest.mark.parametrize(
    ("keys", "type_name", "of_element", "expected"),
    [
        ((("CEReactions", ()), ("Reflect", ())), "DOMString", True, True),
        ((("CEReactions", ()), ("Reflect", ("aria_label",)), ("MeasureAs", ("x",))), "DOMString?", True, True),
        ((("CEReactions", ()), ("Reflect", ())), "boolean", True, True),
        ((("CEReactions", ()), ("Reflect", ())), "[LegacyNullToEmptyString] DOMString", True, True),
        ((("CEReactions", ()), ("Reflect", ()), ("ReflectOnly", ("a",))), "DOMString", True, True),
        ((("CEReactions", ()), ("Reflect", ()), ("URL", ())), "USVString", True, False),
        ((("CEReactions", ()), ("Reflect", ())), "long", True, False),
        ((("Reflect", ()),), "DOMString", True, False),
        ((("CEReactions", ()), ("Reflect", ()), ("Replaceable", ())), "DOMString", True, False),
        ((("CEReactions", ()), ("Reflect", ())), "DOMString", False, False),
    ],
)
def test_a_shared_reflected_setter_checks_its_argument(
    keys: tuple[tuple[str, tuple[str, ...]], ...], type_name: str, of_element: bool, expected: bool
) -> None:
    # bind_gen's optimize_element_cereactions_reflect() and PerformAttributeSetCEReactionsReflect().
    attribute = Attribute("name", _TYPES_BY_NAME[type_name], False, False, ExtendedAttributes(keys), ALWAYS)

    assert checks_setter_arguments(attribute, of_element) is expected


def test_the_cpp_types_of_what_permissions_and_geolocation_take() -> None:
    types = TypeMapper(frozenset(), frozenset())
    descriptor = IdlType(TypeKind.OBJECT, "object")
    timeout = IdlType(TypeKind.INTEGER, "unsigned long", extended_attributes=ExtendedAttributes((("Clamp", ()),)))
    gamepads = IdlType(
        TypeKind.SEQUENCE, "sequence<Gamepad?>", members=(IdlType(TypeKind.INTERFACE, "Gamepad", nullable=True),)
    )
    error_callback = IdlType(TypeKind.CALLBACK_FUNCTION, "PositionErrorCallback", nullable=True)

    # NativeValueTraits<IDLObject>: an object, or a TypeError.
    assert types.argument_type(descriptor, "a") == "v8::Local<v8::Object>"
    assert types.from_v8(descriptor, "value", "permission", Site(fail="return;", argument_index=0), "a")[:2] == [
        "if (!value->IsObject()) {",
        '  exception_state.ThrowTypeError(ExceptionMessages::ArgumentNotOfType(0, "object"));',
    ]
    # [Clamp] converts as Blink's ToSmallerInt() and kin do with it.
    assert types.argument_type(timeout, "a") == "uint32_t"
    assert "if (!FromV8ClampedInteger(isolate, value, &timeout)) {" in types.from_v8(
        timeout, "value", "timeout", Site(fail="return;", dictionary="PositionOptions", member="timeout"), "a"
    )
    assert types.default_value(timeout, "0xFFFFFFFF", "a") == "0xFFFFFFFF"
    # A list of objects or nulls, as navigator.getGamepads() gives with no gamepad.
    assert types.return_type(gamepads, "a") == "std::vector<bindings::ScriptWrappable*>"
    assert types.to_v8(gamepads, "x") == "ToV8Array(isolate, x)"
    assert types.default_value(error_callback, "null", "a") == "v8::Local<v8::Function>()"


def test_a_nullable_callback_object_whose_default_is_null_is_empty() -> None:
    # webkitRequestFileSystem()'s error callback, a callback interface.
    types = TypeMapper(frozenset(), frozenset())
    error_callback = IdlType(TypeKind.CALLBACK_INTERFACE, "ErrorCallback", nullable=True)

    assert types.argument_type(error_callback, "a") == "v8::Local<v8::Object>"
    assert types.default_value(error_callback, "null", "a") == "v8::Local<v8::Object>()"


def test_a_nullable_reflected_attribute_reads_its_keywords() -> None:
    def attribute(*keys: tuple[str, tuple[str, ...]]) -> Attribute:
        idl_type = IdlType(TypeKind.STRING, "DOMString", nullable=True)
        return Attribute("crossOrigin", idl_type, False, False, ExtendedAttributes(keys), ALWAYS)

    limited = attribute(
        ("Reflect", ()),
        ("ReflectOnly", ("anonymous", "use-credentials")),
        ("ReflectInvalid", ("anonymous",)),
        ("ReflectEmpty", ("anonymous",)),
    )

    # bind_gen's make_reflect_process_keyword_state() reads what its keywords and defaults say.
    assert reflected_attribute(limited) == "crossorigin"
    assert reflected_keywords(limited) == Keywords(("anonymous", "use-credentials"), None, "anonymous", "anonymous")
    assert reflection_kind(attribute(("Reflect", ()))) is ReflectionKind.NULLABLE_STRING
    assert reflected_keywords(attribute(("Reflect", ()))) is None


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


def test_an_enforce_range_integer_and_an_item_of_a_sequence_convert_as_blinks_do() -> None:
    types = TypeMapper(frozenset({"AbortSignal"}), frozenset())
    milliseconds = IdlType(TypeKind.INTEGER, "unsigned long long", extended_attributes=_extended("EnforceRange"))
    signals = IdlType(TypeKind.SEQUENCE, "sequence<AbortSignal>", members=(IdlType(TypeKind.INTERFACE, "AbortSignal"),))

    # AbortSignal.timeout()'s argument: Web IDL's range checks, for a 64-bit type too.
    assert types.argument_type(milliseconds, "a") == "uint64_t"
    converted = "\n".join(types.from_v8(milliseconds, "info[0]", "ms", Site(fail="return;", argument_index=0), "a"))
    assert 'FromV8IntegerEnforceRange(isolate, info[0], "unsigned long long", exception_state, &ms)' in converted
    # An item has NativeValue()'s message, as AbortSignal.any([{}]) throws in Chrome.
    items = "\n".join(types.from_v8(signals, "info[0]", "list", Site(fail="return;", argument_index=0), "a"))
    assert 'ExceptionMessages::FailedToConvertJSValue("AbortSignal")' in items
    # [Clamp] converts the integers of 32 bits or fewer only, so a 64-bit one with it is refused.
    with pytest.raises(UnhandledError, match=r"does not handle an argument of type unsigned long long"):
        types.argument_type(dataclasses.replace(milliseconds, extended_attributes=_extended("Clamp")), "a")


def test_the_cpp_types_of_what_text_encoder_and_text_decoder_take_and_give() -> None:
    types = TypeMapper(frozenset(), frozenset({"TextEncoderEncodeIntoResult"}))
    uint8_array = IdlType(TypeKind.BUFFER_SOURCE, "Uint8Array")
    buffer_source = IdlType(
        TypeKind.UNION,
        "ArrayBufferView or ArrayBuffer",
        members=(IdlType(TypeKind.BUFFER_SOURCE, "ArrayBufferView"), IdlType(TypeKind.BUFFER_SOURCE, "ArrayBuffer")),
        extended_attributes=_extended("PassAsSpan"),
    )
    dictionary = IdlType(TypeKind.DICTIONARY, "TextEncoderEncodeIntoResult")

    # A view, which the class may write into, and the bytes of a [PassAsSpan] BufferSource.
    assert types.argument_type(uint8_array, "a") == "v8::Local<v8::Uint8Array>"
    assert types.return_type(uint8_array, "a") == "v8::Local<v8::Uint8Array>"
    assert types.argument_type(buffer_source, "a") == "base::span<const uint8_t>"
    view = "\n".join(types.from_v8(uint8_array, "info[1]", "view", Site(fail="return;", argument_index=1), "a"))
    assert "if (!info[1]->IsUint8Array()) {" in view
    assert 'ExceptionMessages::ArgumentNotOfType(1, "Uint8Array")' in view
    assert 'CheckArrayBufferViewNotShared(view, "Uint8Array", exception_state)' in view
    span = types.from_v8(buffer_source, "info[0]", "bytes", Site(fail="return;", argument_index=0), "a")
    assert span[0] == "ByteSpanArgument bytes;"
    assert "FromV8ByteSpan(info[0], 0, exception_state, &bytes)" in span[1]
    # The class gets the bytes once every argument has converted, which may have detached their buffer.
    assert types.value_type(buffer_source, "a") == "ByteSpanArgument"
    assert types.is_byte_span(buffer_source)
    # The class makes a dictionary's object, and returns it as it is.
    assert types.return_type(dictionary, "a") == "v8::Local<v8::Object>"
    assert types.to_v8(dictionary, "result") == "result"
    # [PassAsSpan] goes with an argument alone, and with no other annotation.
    with pytest.raises(UnhandledError, match="does not handle a value of type"):
        types.from_v8(buffer_source, "value", "bytes", Site(fail="return false;", dictionary="D", member="m"), "a")
    shared = dataclasses.replace(buffer_source, extended_attributes=_extended("PassAsSpan", "AllowShared"))
    with pytest.raises(UnhandledError, match=r"does not handle \[PassAsSpan\]"):
        types.argument_type(shared, "a")


def test_the_cpp_types_of_what_crypto_takes_and_returns() -> None:
    types = TypeMapper(frozenset(), frozenset())
    view = IdlType(TypeKind.BUFFER_SOURCE, "ArrayBufferView")
    quota = IdlType(TypeKind.FLOATING_POINT, "double", nullable=True)

    # A view goes to the class and back as it is, once Blink's checks of NotShared<DOMArrayBufferView> pass.
    assert types.argument_type(view, "a") == "v8::Local<v8::ArrayBufferView>"
    assert types.return_type(view, "a") == "v8::Local<v8::ArrayBufferView>"
    assert types.to_v8(view, "x") == "x"
    lines = types.from_v8(view, "value", "array", Site(fail="return;", argument_index=1), "a")
    assert lines[:3] == [
        "if (!value->IsArrayBufferView()) {",
        '  exception_state.ThrowTypeError(ExceptionMessages::ArgumentNotOfType(1, "ArrayBufferView"));',
        "  return;",
    ]
    assert 'if (!CheckArrayBufferViewNotShared(array, "ArrayBufferView", exception_state)) {' in lines
    # A nullable double is null or a Number.
    assert types.return_type(quota, "a") == "std::optional<double>"
    assert types.to_v8(quota, "x") == "ToV8NullableNumber(isolate, x)"
    with pytest.raises(UnhandledError, match="does not handle an argument of type"):
        types.argument_type(dataclasses.replace(view, nullable=True), "a")


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


def _window(*constructs: ExposedConstruct) -> Interface:
    return _interface(
        name="Window",
        extended_attributes=_extended("Global"),
        attributes=(_attribute("window"),),
        operation_groups=(_operation("alert"),),
        exposed=constructs,
    )


def test_a_global_objects_exposed_constructs_come_between_its_constants_and_operations() -> None:
    # bind_gen installs the attributes, then the constants, then the exposed constructs, then the operations, and puts
    # those that each context decides on the global object later.
    secure = Condition(other=("SecureContext",), context_dependent=True)
    window = _window(
        ExposedConstruct("Node", "interface", "Node", ALWAYS),
        ExposedConstruct("Cache", "interface", "Cache", secure),
        ExposedConstruct("CSS", "namespace", "CSS", ALWAYS),
    )

    plan = plan_interface(window)

    assert [(p.target, p.name, p.phase) for p in plan if p.target == "instance"] == [
        ("instance", "window", Phase.TEMPLATE),
        ("instance", "Node", Phase.TEMPLATE),
        ("instance", "CSS", Phase.TEMPLATE),
        ("instance", "alert", Phase.TEMPLATE),
        ("instance", "Cache", Phase.CONTEXT),
    ]
    node = next(p for p in plan if p.name == "Node")
    assert (node.kind, node.enumerable, node.configurable, node.writable) == ("construct", False, True, True)


def test_a_window_behind_a_feature_exposes_its_featured_constructs_with_each_context() -> None:
    # Window's objects come from Chrome's context snapshot, which holds what is always there.
    window = _window(ExposedConstruct("Gated", "interface", "Gated", Condition(features=("F",))))

    gated = next(p for p in plan_interface(window) if p.name == "Gated")

    assert gated.phase is Phase.CONTEXT


def test_namespaces_and_callback_interfaces_have_constants_on_their_object_only() -> None:
    constant = Constant("ONE", IdlType(TypeKind.INTEGER, "unsigned short"), 1, ExtendedAttributes(), ALWAYS)

    namespace = plan_interface(_interface(kind="namespace", constants=(constant,)))
    callback = plan_interface(_interface(kind="callback interface", constants=(constant,)))

    assert _names(namespace) == ["interface.@@toStringTag", "interface.ONE"]
    assert _names(callback) == ["interface.ONE"]


def test_an_operation_is_installed_where_any_of_its_overloads_is() -> None:
    either = Condition(alternatives=(Condition(features=("A",)), Condition(other=("Exposed=Worker",))))
    always = Condition(alternatives=(Condition(features=("A",)), ALWAYS))

    assert condition_test(either, "Thing.f") == "((features.IsEnabled(RuntimeFeature::kA)))"
    assert condition_test(always, "Thing.f") == "true"
    assert either.describe() == "([RuntimeEnabled=A]) or ([Exposed=Worker])"


def test_the_listing_names_what_each_exposed_construct_gives() -> None:
    window = _window(
        ExposedConstruct("webkitURL", "alias", "URL", ALWAYS),
        ExposedConstruct("Image", "factory function", "HTMLImageElement", ALWAYS, length=0),
    )

    lines = write_listing([window]).splitlines()

    assert "  Window instance.webkitURL: interface object of URL, not enumerable" in lines
    assert "  Window instance.Image: factory function of HTMLImageElement, length 0, not enumerable" in lines


def _context() -> KnownGapContext:
    return KnownGapContext(_types(), _gaps(set(), {"Thing"}, {"Thing": None}))


def test_a_table_holds_each_member_in_the_plans_order_with_its_condition() -> None:
    interface = _interface(
        attributes=(_attribute("on", readonly=False), _attribute("gated", condition=Condition(features=("F",)))),
        operation_groups=(_operation("run", length=2),),
        constructor_group=_operation("constructor", length=1),
    )
    construct = Construct(interface, 7, None, in_global_prototype_chain=False)

    files = write_tables([construct], 2, lambda name: False, condition_test, _context())

    source = files["interfaces_without_class_0.cc"]
    rows = [line.strip() for line in source.splitlines() if line.startswith('    {"')]
    assert [row.split(",")[0] for row in rows] == ['{"on"', '{"run"', '{"gated"']
    assert "kMemberHasSetter" in rows[0]
    assert "&Condition0" in rows[2]
    assert "return features.IsEnabled(RuntimeFeature::kF);" in source
    assert "ConstructorKind::kKnownGap" in source
    assert "    7,\n    &kThingTable," in source
    assert files["interfaces_without_class_1.cc"].count("const WrapperTypeInfo") == 0
    assert "class V8Thing final {" in files["interfaces_without_class.h"]


def test_a_table_leaves_out_what_a_page_never_installs() -> None:
    interface = _interface(attributes=(_attribute("isolated", condition=Condition(other=("IsolatedContext",))),))

    source = write_tables([Construct(interface, 0, None, False)], 1, lambda name: False, condition_test, _context())[
        "interfaces_without_class_0.cc"
    ]

    assert '"isolated"' not in source


def test_iterator_symbols_can_be_replaced_and_the_others_cannot() -> None:
    # Blink installs @@iterator with v8::DontEnum alone, and @@toStringTag and @@unscopables read-only.
    pair = CollectionLike("iterable", (), (_operation("entries"),), "entries")
    indexed = _interface(special_operations=(SpecialOperation("indexed getter", _operation("item").overloads[0]),))
    iterable = _interface(collection=pair, unscopables=("x",))

    def writable(interface: Interface) -> dict[str, bool | None]:
        return {p.name: p.writable for p in plan_interface(interface) if p.name.startswith("@@")}

    assert writable(indexed) == {"@@toStringTag": False, "@@iterator": True}
    assert writable(iterable) == {"@@toStringTag": False, "@@unscopables": False, "@@iterator": True}


def _gaps(classes: set[str], bindings: set[str], parents: dict[str, str | None]) -> KnownGaps:
    return KnownGaps(parents, classes.__contains__, bindings.__contains__)


def test_a_known_gap_keeps_todays_error_where_its_default_would_be_unsafe() -> None:
    # Node has a class, Attr inherits from it, AbortSignal's class behind is EventTarget's, Worklet is a worker's.
    parents = {"Node": None, "Attr": "Node", "EventTarget": None, "AbortSignal": "EventTarget", "Plain": None}
    gaps = _gaps({"Node", "EventTarget"}, {"Node", "Attr", "EventTarget", "AbortSignal", "Plain"}, parents)

    def interface(name: str, nullable: bool = False) -> IdlType:
        return IdlType(TypeKind.INTERFACE, name, nullable=nullable)

    assert [gaps.default_is_safe(interface(name)) for name in ("Node", "Attr", "AbortSignal", "Plain")] == [
        False,
        False,
        True,
        True,
    ]
    assert gaps.default_is_safe(interface("Node", nullable=True))
    assert not gaps.default_is_safe(interface("Worklet"))
    assert gaps.default_is_safe(IdlType(TypeKind.SEQUENCE, "sequence", members=(interface("Node"),)))
    assert not gaps.default_is_safe(IdlType(TypeKind.PROMISE, "Promise", members=(interface("Attr"),)))
    assert gaps.default_is_safe(IdlType(TypeKind.UNION, "x", members=(interface("Node"), interface("Plain"))))
    assert gaps.refuses(_interface(name="SubtleCrypto"), _attribute("digest"))
    assert not gaps.refuses(_interface(name="Crypto"), _attribute("subtle"))
    assert gaps.refuses(_interface(name="Attr"), None)
    assert not gaps.refuses(_interface(name="AbortSignal"), None)


def test_event_handler_attributes_are_not_known_gaps() -> None:
    handler = IdlType(TypeKind.CALLBACK_FUNCTION, "EventHandlerNonNull", nullable=True)

    assert KnownGaps.is_event_handler(Attribute("onabort", handler, False, False, ExtendedAttributes(), ALWAYS))
    assert not KnownGaps.is_event_handler(_attribute("onabort"))


def _types(dictionaries: dict[str, Dictionary] | None = None) -> TypeTables:
    return TypeTables(
        dictionaries or {},
        {"Mode": ("a", "b")},
        frozenset({"EventHandlerNonNull"}),
        lambda name: f"&V8{name}::kWrapperTypeInfo" if name != "Unknown" else None,
        condition_test,
    )


def test_each_distinct_type_gets_one_table_with_chromes_name_for_it() -> None:
    types = _types()
    long_type = IdlType(TypeKind.INTEGER, "long", extended_attributes=_extended("EnforceRange"))
    union = IdlType(
        TypeKind.UNION, "x", members=(IdlType(TypeKind.STRING, "DOMString"), IdlType(TypeKind.INTERFACE, "Blob"))
    )

    first = types.type(long_type, "a")

    assert types.type(long_type, "b") == first
    assert types.type(IdlType(TypeKind.INTEGER, "long"), "c") != first
    types.type(union, "d")
    types.type(IdlType(TypeKind.ENUMERATION, "Mode"), "e")
    types.type(IdlType(TypeKind.CALLBACK_FUNCTION, "EventHandlerNonNull", nullable=True), "f")
    types.type(IdlType(TypeKind.INTERFACE, "Unknown"), "g")
    _, source = types.write(set())
    assert '"long", IdlTypeKind::kLong, kIdlTypeEnforceRange, nullptr' in source
    assert '"(Blob or DOMString)", IdlTypeKind::kUnion' in source
    assert "constexpr const char* kValues" in source
    assert '{"a", "b"}' in source
    assert "kIdlTypeLegacyTreatNonObjectAsNull | kIdlTypeNullable" in source
    assert '"Unknown", IdlTypeKind::kInterface, 0, nullptr' in source


def test_a_dictionary_table_holds_its_members_with_their_defaults() -> None:
    member = DictionaryMember("once", BOOLEAN, False, "false", ExtendedAttributes(), ALWAYS)
    required = DictionaryMember("type", IdlType(TypeKind.STRING, "DOMString"), True, None, ExtendedAttributes(), ALWAYS)
    types = _types({"Options": Dictionary("Options", None, (member, required), ExtendedAttributes())})

    types.type(IdlType(TypeKind.DICTIONARY, "Options"), "a")

    header, source = types.write(set())
    assert "extern const DictionaryInfo kDictionary0;" in header
    assert '{"once", &kIdlType' in source
    assert ', nullptr, false, "false"},' in source
    assert ", nullptr, true, nullptr}," in source


def test_an_operation_table_holds_each_overload_with_its_arguments() -> None:
    optional = Argument("x", BOOLEAN, True, False, None)
    rest = Argument("rest", IdlType(TypeKind.ANY, "any"), False, True, None)
    overloads = (
        Operation("f", (optional,), UNDEFINED, False, ExtendedAttributes(), ALWAYS),
        Operation("f", (rest,), BOOLEAN, False, ExtendedAttributes(), Condition(features=("F",))),
    )
    group = OperationGroup("f", overloads, False, 0, ExtendedAttributes(), ALWAYS)
    types = _types()

    reference = types.operation(group, "Thing.f")

    _, source = types.write(set())
    assert reference == "&kOperation0"
    assert "constexpr ArgumentInfo kArguments0_0[] = {{&kIdlType0, kArgumentOptional}};" in source
    assert "kArgumentVariadic" in source
    assert "kOverloads0[] = {{kArguments0_0, &kIdlType1, nullptr}, {kArguments0_1, &kIdlType0, &Condition0}};" in (
        source
    )


def _collection(*specials: str, unenumerable: bool = True) -> Interface:
    """An interface like PluginArray, with `length`, item() and namedItem(), and the special operations named."""
    thing = IdlType(TypeKind.INTERFACE, "Thing", nullable=True)
    item = Operation(
        "item",
        (Argument("index", IdlType(TypeKind.INTEGER, "unsigned long"), False, False, None),),
        thing,
        False,
        ExtendedAttributes(),
        ALWAYS,
    )
    named_item = Operation(
        "namedItem",
        (Argument("name", IdlType(TypeKind.STRING, "DOMString"), False, False, None),),
        thing,
        False,
        ExtendedAttributes(),
        ALWAYS,
    )
    operations = {"indexed getter": item, "named getter": named_item, "named setter": named_item}
    return _interface(
        attributes=(
            Attribute("length", IdlType(TypeKind.INTEGER, "unsigned long"), True, False, ExtendedAttributes(), ALWAYS),
        ),
        operation_groups=tuple(
            OperationGroup(operation.name, (operation,), False, 1, ExtendedAttributes(), ALWAYS)
            for operation in (item, named_item)
        ),
        special_operations=tuple(SpecialOperation(kind, operations[kind]) for kind in specials),
        extended_attributes=_extended("LegacyUnenumerableNamedProperties") if unenumerable else ExtendedAttributes(),
    )


def _bindings_of(
    interface: Interface, implemented: ImplementedInterface, dictionaries: dict[str, Dictionary] | None = None
) -> str:
    """The bindings that the C++ writer gives the interface, beside a Window that exposes it."""
    window = _interface(
        name="Window",
        tag=2,
        max_subclass_tag=2,
        extended_attributes=_extended("Global"),
        exposed_constructs=("Thing", "Window"),
        exposed=tuple(ExposedConstruct(name, "interface", name, ALWAYS) for name in ("Thing", "Window")),
    )
    definitions = Definitions({"Thing": interface, "Window": window}, dictionaries or {}, {}, {}, frozenset())
    files = write_bindings(definitions, [], [implemented, ImplementedInterface("Window", "mitto/dom/window.h")])
    return files["v8_thing.cc"]


def test_overloads_that_return_promises_choose_a_dictionary_for_undefined_null_and_objects() -> None:
    # CookieStore's get(): a string or optional options, and an error in either rejects the promise.
    promise = IdlType(TypeKind.PROMISE, "Promise", members=(IdlType(TypeKind.ANY, "any"),))
    by_name = Operation(
        "get",
        (Argument("name", IdlType(TypeKind.STRING, "USVString"), False, False, None),),
        promise,
        False,
        _extended("RaisesException"),
        ALWAYS,
    )
    by_options = Operation(
        "get",
        (Argument("options", IdlType(TypeKind.DICTIONARY, "Options"), True, False, "{}"),),
        promise,
        False,
        _extended("RaisesException"),
        ALWAYS,
    )
    member = DictionaryMember("name", IdlType(TypeKind.STRING, "USVString"), False, None, ExtendedAttributes(), ALWAYS)
    interface = _interface(
        operation_groups=(OperationGroup("get", (by_name, by_options), False, 0, ExtendedAttributes(), ALWAYS),)
    )

    source = _bindings_of(
        interface,
        ImplementedInterface("Thing", "mitto/dom/thing.h", implemented=("get",)),
        {"Options": Dictionary("Options", None, (member,), ExtendedAttributes())},
    )

    assert "ExceptionToRejectPromiseScope reject_promise_scope(info);" in source
    assert "if (info[0]->IsUndefined() || info[0]->IsNull() || info[0]->IsObject()) {" in source
    assert "info.Length() <" not in source


def test_overloads_that_return_promises_count_the_arguments_before_checking_the_receiver() -> None:
    # As CookieStore's set() and delete(): a call of too few arguments on another object rejects for the arguments.
    promise = IdlType(TypeKind.PROMISE, "Promise", members=(IdlType(TypeKind.ANY, "any"),))
    name = Argument("name", IdlType(TypeKind.STRING, "DOMString"), False, False, None)
    flag = Argument("flag", BOOLEAN, False, False, None)
    overloads = (
        Operation("request", (name,), promise, False, ExtendedAttributes(), ALWAYS),
        Operation("request", (name, flag), promise, False, ExtendedAttributes(), ALWAYS),
    )
    interface = _interface(
        operation_groups=(OperationGroup("request", overloads, False, 1, ExtendedAttributes(), ALWAYS),)
    )

    source = _bindings_of(interface, ImplementedInterface("Thing", "mitto/dom/thing.h", implemented=("request",)))

    body = source[source.index("void RequestOperationCallback") :]
    assert body.index("if (info.Length() < 1) {") < body.index("Illegal invocation")


def test_constructors_of_a_string_and_a_sequence_choose_the_sequence_for_an_iterable_object() -> None:
    # PresentationRequest's constructor(USVString url) and constructor(sequence<USVString> urls).
    url = Argument("url", IdlType(TypeKind.STRING, "USVString"), False, False, None)
    strings = IdlType(TypeKind.SEQUENCE, "sequence<USVString>", members=(IdlType(TypeKind.STRING, "USVString"),))
    urls = Argument("urls", strings, False, False, None)
    raises = _extended("RaisesException")
    constructors = OperationGroup(
        "constructor",
        (
            Operation("constructor", (url,), UNDEFINED, False, raises, ALWAYS),
            Operation("constructor", (urls,), UNDEFINED, False, raises, ALWAYS),
        ),
        False,
        1,
        ExtendedAttributes(),
        ALWAYS,
    )

    source = _bindings_of(
        _interface(constructor_group=constructors), ImplementedInterface("Thing", "mitto/dom/thing.h")
    )

    body = source[source.index("void ConstructorCallback") :]
    # bind_gen's dispatcher: an array or an object with an @@iterator, whose reading may throw, then the string.
    assert "if (info.Length() < 1) {" in body
    assert "if (info[0]->IsArray() || IsIterableObject(isolate, info[0], &iterator_threw)) {" in body
    assert body.index("IsIterableObject") < body.index("if (iterator_threw) {") < body.rindex("FromV8USVString")
    assert body.count("mitto::Thing* const impl = mitto::Thing::Create(") == 2


def test_a_class_with_a_window_alias_gives_the_window_both_names() -> None:
    # PermissionsPolicy's [LegacyWindowAlias=FeaturePolicy]: the alias is the same interface object.
    thing = _interface(extended_attributes=ExtendedAttributes((("LegacyWindowAlias", ("OldThing",)),)))
    exposed = (
        ExposedConstruct("Thing", "interface", "Thing", ALWAYS),
        ExposedConstruct("Window", "interface", "Window", ALWAYS),
        ExposedConstruct("OldThing", "alias", "Thing", ALWAYS),
    )
    window = _interface(
        name="Window",
        tag=2,
        max_subclass_tag=2,
        extended_attributes=_extended("Global"),
        exposed_constructs=("Thing", "Window"),
        exposed=exposed,
    )
    definitions = Definitions({"Thing": thing, "Window": window}, {}, {}, {}, frozenset())
    implemented = [
        ImplementedInterface("Thing", "mitto/dom/thing.h"),
        ImplementedInterface("Window", "mitto/dom/window.h"),
    ]

    source = write_bindings(definitions, [], implemented)["v8_window.cc"]

    assert '{"Thing", &V8Thing::kWrapperTypeInfo, ExposedConstructInfo::Kind::kInterfaceObject' in source
    assert '{"OldThing", &V8Thing::kWrapperTypeInfo, ExposedConstructInfo::Kind::kInterfaceObject' in source


def test_a_class_stringifier_gives_its_attributes_getter() -> None:
    # DOMTokenList's `stringifier attribute DOMString value`: Blink's toString() is the getter.
    value = Attribute("value", IdlType(TypeKind.STRING, "DOMString"), False, False, ExtendedAttributes(), ALWAYS)
    thing = _interface(attributes=(value,), stringifier="value")

    source = _bindings_of(thing, ImplementedInterface("Thing", "mitto/dom/thing.h"))

    callback = source[source.index("void ToStringOperationCallback") :]
    assert "impl->Value()" in callback[: callback.index("\n}")]
    assert '"toString", kInterfaceName, ToStringOperationCallback, 0' in source


def test_a_put_forwards_setter_sets_the_forwarded_attribute_through_v8() -> None:
    # Element's [PutForwards=value] classList: bind_gen's make_steps_of_put_forwards().
    list_type = IdlType(TypeKind.INTERFACE, "Thing")
    forwards = ExtendedAttributes((("PutForwards", ("value",)),))
    attribute = Attribute("classList", list_type, True, False, forwards, ALWAYS)
    thing = _interface(attributes=(attribute,))

    source = _bindings_of(thing, ImplementedInterface("Thing", "mitto/dom/thing.h"))

    setter = source[source.index("void ClassListAttributeSetCallback") :]
    setter = setter[: setter.index("\n}")]
    assert 'v8::String::NewFromUtf8Literal(isolate, "classList")' in setter
    assert '"The attribute value is not an object"' in setter
    assert 'v8::String::NewFromUtf8Literal(isolate, "value"), info[0]' in setter


def test_a_factory_function_with_behaviour_calls_the_class_and_the_window_installs_it() -> None:
    # HTMLImageElement's [LegacyFactoryFunction=Image(optional unsigned long width, optional unsigned long height)].
    width = Argument("width", IdlType(TypeKind.INTEGER, "unsigned long"), True, False, None)
    height = Argument("height", IdlType(TypeKind.INTEGER, "unsigned long"), True, False, None)
    image = Operation(
        "Image", (width, height), IdlType(TypeKind.INTERFACE, "Thing"), False, ExtendedAttributes(), ALWAYS
    )
    group = OperationGroup("Image", (image,), False, 0, ExtendedAttributes(), ALWAYS)
    thing = _interface(legacy_factory_functions=(group,))
    exposed = (
        ExposedConstruct("Thing", "interface", "Thing", ALWAYS),
        ExposedConstruct("Window", "interface", "Window", ALWAYS),
        ExposedConstruct("Image", "factory function", "Thing", ALWAYS, length=0),
    )
    window = _interface(
        name="Window",
        tag=2,
        max_subclass_tag=2,
        extended_attributes=_extended("Global"),
        exposed_constructs=("Thing", "Window"),
        exposed=exposed,
    )
    definitions = Definitions({"Thing": thing, "Window": window}, {}, {}, {}, frozenset())
    implemented = [
        ImplementedInterface("Thing", "mitto/dom/thing.h", implemented=("Image",)),
        ImplementedInterface("Window", "mitto/dom/window.h"),
    ]

    files = write_bindings(definitions, [], implemented)

    assert "static void ImageLegacyFactoryFunctionCallback(" in files["v8_thing.h"]
    source = files["v8_thing.cc"]
    assert "mitto::Thing::CreateForJSConstructor(arg1_width, arg2_height)" in source
    assert "ExceptionMessages::ConstructorCalledAsFunction()" in source
    assert "&V8Thing::ImageLegacyFactoryFunctionCallback}" in files["v8_window.cc"]


def test_a_class_declared_in_a_shared_header_is_included_from_it() -> None:
    # svg_elements.h declares the classes of SVG that have nothing of their own, which interfaces.gni's `header` names.
    part = _interface(name="Part", tag=3, max_subclass_tag=3)
    thing = _interface(
        attributes=(Attribute("part", IdlType(TypeKind.INTERFACE, "Part"), True, False, ExtendedAttributes(), ALWAYS),)
    )
    window = _interface(
        name="Window",
        tag=2,
        max_subclass_tag=2,
        extended_attributes=_extended("Global"),
        exposed_constructs=("Part", "Thing", "Window"),
        exposed=tuple(ExposedConstruct(name, "interface", name, ALWAYS) for name in ("Part", "Thing", "Window")),
    )
    definitions = Definitions({"Part": part, "Thing": thing, "Window": window}, {}, {}, {}, frozenset())
    implemented = [
        ImplementedInterface("Part", "mitto/dom/parts.h"),
        ImplementedInterface("Thing", "mitto/dom/thing.h"),
        ImplementedInterface("Window", "mitto/dom/window.h"),
    ]

    files = write_bindings(definitions, [], implemented)

    assert '#include "mitto/dom/parts.h"' in files["v8_part.cc"]
    assert '#include "mitto/dom/parts.h"' in files["v8_thing.cc"]
    assert "mitto/dom/part.h" not in files["v8_thing.cc"]


def test_a_class_with_indexed_and_named_getters_has_interceptors_that_call_it() -> None:
    source = _bindings_of(
        _collection("indexed getter", "named getter"), ImplementedInterface("Thing", "mitto/dom/thing.h")
    )

    assert "return HolderOf(isolate, holder)->Length();" in source
    assert "return ToV8(isolate, HolderOf(isolate, holder)->Item(index));" in source
    assert "const auto result = HolderOf(isolate, holder)->NamedItem(name);" in source
    assert "return HolderOf(isolate, holder)->NamedPropertyEnumerator();" in source
    assert "return HolderOf(isolate, holder)->NamedPropertyQuery(name);" in source
    assert "    kNamedPropertiesUnenumerable,\n};" in source
    assert "InstallIndexedAndNamedProperties(isolate, instance_template, kIndexedAndNamedProperties);" in source
    # %Array.prototype.values%, as Blink installs it for an indexed getter.
    assert "SetIntrinsicDataProperty(v8::Symbol::GetIterator(isolate), v8::kArrayProto_values," in source


def test_a_named_getter_alone_leaves_the_indices_to_it() -> None:
    source = _bindings_of(
        _collection("named getter", unenumerable=False), ImplementedInterface("Thing", "mitto/dom/thing.h")
    )

    assert "kIndexedAndNamedProperties = {\n    nullptr,\n    nullptr,\n    &NamedPropertyGetter," in source
    assert "    0,\n};" in source
    assert "kArrayProto_values" not in source


def test_a_getter_that_is_a_known_gap_has_no_interceptor() -> None:
    implemented = ImplementedInterface("Thing", "mitto/dom/thing.h", implemented=("length", "item", "namedItem"))

    source = _bindings_of(_collection("indexed getter", "named getter"), implemented)

    assert "InstallIndexedAndNamedProperties" not in source
    assert "kArrayProto_values" in source


def test_the_writer_refuses_what_it_cannot_install_of_named_properties() -> None:
    with pytest.raises(UnhandledError, match="interface Thing: the C\\+\\+ writer does not handle its named setter"):
        _bindings_of(_collection("named getter", "named setter"), ImplementedInterface("Thing", "mitto/dom/thing.h"))


def test_each_iterable_gets_an_iterator_of_its_own() -> None:
    # Blink's database merges an iterable's iterator into the interface, so the generator writes its table itself.
    pair = CollectionLike("iterable", (), (_operation("entries"),), "entries", iterator="SyncIterator_Thing")
    streamed = CollectionLike(
        "async iterable", (), (_operation("values"),), "values", iterator="AsyncIterator_Stream", has_return=True
    )
    window = _window(ExposedConstruct("Thing", "interface", "Thing", ALWAYS))
    interfaces = {
        "Window": window,
        "Thing": _interface(collection=pair),
        "Stream": _interface(name="Stream", collection=streamed),
    }
    definitions = Definitions(interfaces, {}, {}, {}, frozenset())

    iterators = cpp_writer._iterators(definitions, ["Thing", "Stream"])  # pyright: ignore[reportPrivateUsage]

    thing, thing_name, thing_parent = iterators["SyncIterator_Thing"]
    stream, stream_name, stream_parent = iterators["AsyncIterator_Stream"]
    assert (thing.kind, thing_name, thing_parent) == ("sync iterator", "Thing Iterator", "v8::kIteratorPrototype")
    assert [group.name for group in thing.operation_groups] == ["next"]
    assert (stream_name, stream_parent) == ("Stream AsyncIterator", "v8::kAsyncIteratorPrototype")
    assert [group.name for group in stream.operation_groups] == ["next", "return"]


def test_the_default_tojson_steps_read_json_attributes_of_each_interface_that_declares_tojson() -> None:
    to_json = Operation("toJSON", (), IdlType(TypeKind.OBJECT, "object"), False, ExtendedAttributes(), ALWAYS)
    group = OperationGroup("toJSON", (to_json,), False, 0, ExtendedAttributes(), ALWAYS)

    def attribute(name: str, idl_type: IdlType) -> Attribute:
        return Attribute(name, idl_type, True, False, ExtendedAttributes(), ALWAYS)

    long_type = IdlType(TypeKind.INTEGER, "long")
    base = _interface(
        name="Base",
        attributes=(
            attribute("a", long_type),
            attribute("p", IdlType(TypeKind.PROMISE, "Promise", members=(long_type,))),
        ),
        operation_groups=(group,),
    )
    middle = _interface(name="Middle", parent="Base", attributes=(attribute("m", long_type),))
    leaf = _interface(
        name="Leaf",
        parent="Middle",
        attributes=(
            attribute("b", IdlType(TypeKind.INTERFACE, "Base", nullable=True)),
            attribute("n", IdlType(TypeKind.ANY, "any")),
        ),
        operation_groups=(group,),
    )
    interfaces = {interface.name: interface for interface in (base, middle, leaf)}

    assert has_default_tojson(group)
    assert [a.name for a in default_tojson_attributes(leaf, interfaces, {})] == ["a", "b"]
    types = _types()
    assert types.json_attributes(default_tojson_attributes(leaf, interfaces, {}), "Leaf.toJSON") == 0
    _, source = types.write(set())
    assert 'constexpr JsonAttributeInfo kJsonAttributes0[] = {{"a", nullptr}, {"b", nullptr}};' in source
    assert "return kJsonAttributeLists[static_cast<size_t>(index)];" in source


def test_a_known_gap_may_make_an_object_of_a_class_that_makes_one_for_it() -> None:
    # HTMLCollection's class makes an empty one, and HTMLFormControlsCollection, without a class, gets one behind it.
    parents = {"HTMLCollection": None, "HTMLFormControlsCollection": "HTMLCollection", "Node": None, "Attr": "Node"}
    classes = {"HTMLCollection", "Node"}
    gaps = KnownGaps(parents, classes.__contains__, set(parents).__contains__, {"HTMLCollection"}.__contains__)

    def interface(name: str) -> IdlType:
        return IdlType(TypeKind.INTERFACE, name)

    assert gaps.default_is_safe(interface("HTMLCollection"))
    assert gaps.default_is_safe(interface("HTMLFormControlsCollection"))
    assert not gaps.default_is_safe(interface("Node"))
    assert not gaps.default_is_safe(interface("Attr"))
    assert gaps.object_behind("HTMLFormControlsCollection") == "kClass"
    assert gaps.object_behind("Attr") == "kNone"
    assert not gaps.refuses(_interface(name="HTMLCollection"), None)


def _made(*implemented: str, constructor: bool = True) -> str:
    """The bindings of a class that a Known Gap may make, with a constructor that takes a boolean."""
    group = None
    if constructor:
        argument = Argument("on", BOOLEAN, False, False, None)
        operation = Operation("constructor", (argument,), UNDEFINED, False, ExtendedAttributes(), ALWAYS)
        group = OperationGroup("constructor", (operation,), False, 1, ExtendedAttributes(), ALWAYS)
    interface = _interface(attributes=(_attribute("on"),), constructor_group=group)
    implemented_interface = ImplementedInterface(
        "Thing", "mitto/dom/thing.h", implemented=implemented, made_by_known_gaps=True
    )
    return _bindings_of(interface, implemented_interface)


def test_a_class_that_a_known_gap_may_make_says_how_and_may_leave_its_constructor_a_known_gap() -> None:
    source = _made("on")

    assert "  return mitto::Thing::CreateForKnownGap(info);" in source
    assert "    &CreateForKnownGap,\n};" in source
    assert "KnownGapConstructorOfClass(info, V8Thing::kWrapperTypeInfo, *&kOperation0, 0);" in source
    assert "mitto::Thing::Create(" not in source
    assert "mitto::Thing::Create(arg1_on)" in _made("on", "constructor")


def test_a_class_without_it_has_its_constructor_and_makes_nothing_for_known_gaps() -> None:
    argument = Argument("on", BOOLEAN, False, False, None)
    operation = Operation("constructor", (argument,), UNDEFINED, False, ExtendedAttributes(), ALWAYS)
    group = OperationGroup("constructor", (operation,), False, 1, ExtendedAttributes(), ALWAYS)

    source = _bindings_of(
        _interface(constructor_group=group), ImplementedInterface("Thing", "mitto/dom/thing.h", implemented=())
    )

    assert "mitto::Thing::Create(arg1_on)" in source
    assert "    nullptr,\n    nullptr,\n};" in source
    with pytest.raises(UnhandledError, match="Thing has no member constructor"):
        _bindings_of(
            _interface(constructor_group=group),
            ImplementedInterface("Thing", "mitto/dom/thing.h", implemented=("constructor",)),
        )


def test_a_static_attribute_with_save_same_object_is_kept_on_the_interface_object() -> None:
    strings = IdlType(TypeKind.FROZEN_ARRAY, "FrozenArray", members=(IdlType(TypeKind.STRING, "DOMString"),))
    count = IdlType(TypeKind.INTEGER, "unsigned long")
    interface = _interface(
        attributes=(
            Attribute("kinds", strings, True, True, _extended("SameObject", "SaveSameObject"), ALWAYS),
            Attribute("most", count, True, True, ExtendedAttributes(), ALWAYS),
        )
    )

    source = _bindings_of(interface, ImplementedInterface("Thing", "mitto/dom/thing.h"))

    assert 'v8::String::NewFromUtf8Literal(isolate, "mitto:Thing.kinds"));' in source
    assert "  value = mitto::Thing::Kinds();" in source
    assert "info.GetReturnValue().Set(v8::Integer::NewFromUnsigned(isolate, mitto::Thing::Most()));" in source


def test_a_read_only_maplike_with_behaviour_reads_the_classs_entries() -> None:
    string = IdlType(TypeKind.STRING, "DOMString")
    size = Attribute("size", IdlType(TypeKind.INTEGER, "unsigned long"), True, False, ExtendedAttributes(), ALWAYS)
    key = Argument("key", string, False, False, None)
    callback = Argument("callback", IdlType(TypeKind.CALLBACK_FUNCTION, "ForEachIteratorCallback"), False, False, None)
    this_arg = Argument("thisArg", IdlType(TypeKind.ANY, "any"), True, False, None)

    def group(name: str, arguments: tuple[Argument, ...], returns: IdlType) -> OperationGroup:
        operation = Operation(name, arguments, returns, False, ExtendedAttributes(), ALWAYS)
        return OperationGroup(name, (operation,), False, len(arguments), ExtendedAttributes(), ALWAYS)

    iterator = IdlType(TypeKind.ITERATOR, "SyncIterator_Thing")
    maplike = CollectionLike(
        "maplike",
        (size,),
        (
            group("entries", (), iterator),
            group("forEach", (callback, this_arg), UNDEFINED),
            group("get", (key,), IdlType(TypeKind.ANY, "any")),
            group("has", (key,), BOOLEAN),
        ),
        "entries",
        iterator="SyncIterator_Thing",
        key_type=string,
        value_type=IdlType(TypeKind.INTEGER, "unsigned long long"),
    )

    source = _bindings_of(
        _interface(collection=maplike), ImplementedInterface("Thing", "mitto/dom/thing.h", implemented=("maplike",))
    )

    assert "for (const auto& item : impl->MaplikeEntries()) {" in source
    assert "static_cast<uint32_t>(impl->MaplikeEntries().size())" in source
    assert "NewIteratorOverList(context, V8SyncIterator_Thing::kWrapperTypeInfo, items)" in source
    assert "->Call(context, info[1], 3, arguments).IsEmpty()" in source
    assert "info.GetReturnValue().Set(v8::Undefined(isolate));" in source
    assert "info.GetReturnValue().Set(v8::False(isolate));" in source


def test_a_promise_attribute_that_is_a_known_gap_has_no_signature() -> None:
    # So that its getter rejects for a receiver of another kind, as Chrome's "get ready" of FontFaceSet does.
    promise = IdlType(TypeKind.PROMISE, "Promise", members=(UNDEFINED,))
    interface = _interface(
        attributes=(
            Attribute("ready", promise, True, False, ExtendedAttributes(), ALWAYS),
            Attribute("on", BOOLEAN, True, False, ExtendedAttributes(), ALWAYS),
        )
    )

    source = _bindings_of(interface, ImplementedInterface("Thing", "mitto/dom/thing.h", implemented=()))

    lines = [line for line in source.splitlines() if "InstallAttribute(" in line]
    assert "v8::Local<v8::Signature>()" in next(line for line in lines if '"ready"' in line)
    assert 'signature, "on"' in next(line for line in lines if '"on"' in line)
