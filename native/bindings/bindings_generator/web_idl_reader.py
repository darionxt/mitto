"""Reads Blink's Web IDL database into the generator's model, and refuses what the generator does not handle.

It visits every definition and member of the database, every type and every extended attribute, whether or not the
Runtime exposes it yet, so that a construct new in an Engine Release's Web IDL files stops the build with a message
that names it. The database's objects are Blink's own, from third_party/blink/renderer/bindings/scripts/web_idl, which
GN puts on the path: this module is the only one that touches them.

When a member is exposed follows bind_gen's interface.py, which decides it for each member of each interface as a
global object of a Window sees it, since a Page is a window.
"""

# Upstream: third_party/blink/renderer/bindings/scripts/bind_gen/interface.py
# Upstream: third_party/blink/renderer/bindings/scripts/bind_gen/codegen_expr.py
# Upstream: third_party/blink/renderer/bindings/scripts/web_idl/database.py

import dataclasses
import itertools
from typing import Any

from bindings_generator import extended_attributes
from bindings_generator.errors import UnhandledError
from bindings_generator.model import (
    ALWAYS,
    Argument,
    Attribute,
    CallbackInterface,
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

__all__ = ["Definitions", "read_database"]

# The global object that a Page's scripts run in, by the name that [Exposed] gives it.
PAGE_GLOBAL_NAME = "Window"

# The kinds of definition that the database holds, as web_idl's DatabaseBody.Kind names them, and those that the
# reader reads on their own. Mixins are merged into the interfaces that include them, unions and typedefs into the
# types that name them, observable arrays and iterators into their interfaces' members.
_KINDS_READ = frozenset({"interface", "namespace", "callback interface", "dictionary", "enumeration"})
_KINDS_MERGED = frozenset(
    {"interface mixin", "union", "typedef", "observable array", "sync iterator", "async iterator", "callback function"}
)


@dataclasses.dataclass(frozen=True)
class Definitions:
    """Everything the reader took from the database."""

    interfaces: dict[str, Interface]
    """Interfaces, namespaces and callback interfaces, by identifier."""
    dictionaries: dict[str, Dictionary]
    callback_interfaces: dict[str, CallbackInterface]
    enumerations: dict[str, tuple[str, ...]]
    callback_functions: frozenset[str]


def read_database(database: Any) -> Definitions:
    """Reads every definition of a web_idl.Database.

    Raises:
        UnhandledError: if anything in the database is a construct, a type or an extended attribute that the generator
            does not handle.
    """
    # The database has no public list of its kinds.
    body: Any = database._impl
    for kind in body.Kind.values():
        if kind not in _KINDS_READ | _KINDS_MERGED and body.find_by_kind(kind):
            raise UnhandledError(f"the database holds a kind of definition the generator does not handle: {kind}")

    interfaces: dict[str, Interface] = {}
    for definition in itertools.chain(database.interfaces, database.namespaces, database.callback_interfaces):
        interfaces[definition.identifier] = _read_interface(definition)
    callback_interfaces = {
        definition.identifier: _callback_interface(interfaces[definition.identifier])
        for definition in database.callback_interfaces
    }
    for definition in database.callback_functions:
        where = f"callback {definition.identifier}"
        _check_extended_attributes(where, definition)
        _read_type(definition.return_type, f"{where} return type")
        _read_arguments(definition.arguments, where)
    dictionaries = {definition.identifier: _read_dictionary(definition) for definition in database.dictionaries}
    enumerations: dict[str, tuple[str, ...]] = {}
    for definition in database.enumerations:
        _check_extended_attributes(f"enum {definition.identifier}", definition)
        enumerations[definition.identifier] = tuple(definition.values)
    for definition in database.observable_arrays:
        _read_type(definition.element_type, f"observable array {definition.identifier}")
    return Definitions(
        interfaces=interfaces,
        dictionaries=dictionaries,
        callback_interfaces=callback_interfaces,
        enumerations=enumerations,
        callback_functions=frozenset(definition.identifier for definition in database.callback_functions),
    )


def _check_extended_attributes(where: str, construct: Any) -> ExtendedAttributes:
    entries: list[tuple[str, tuple[str, ...]]] = []
    for extended_attribute in construct.extended_attributes:
        # An argument list, such as [Exposed(Window Feature)], has no values but its whole form.
        values = extended_attribute.values if extended_attribute.has_values else (extended_attribute.syntactic_form,)
        entries.append((extended_attribute.key, tuple(values)))
    extended_attributes.check(where, [key for key, _ in entries])
    return ExtendedAttributes(tuple(entries))


def _read_type(idl_type: Any, where: str) -> IdlType:
    """Reads a type, with its typedefs resolved, and the types inside it."""
    nullable = bool(idl_type.is_nullable)
    annotations = _check_extended_attributes(f"type {idl_type.syntactic_form} of {where}", idl_type)
    inner: Any = idl_type.unwrap(nullable=True, typedef=True, variadic=True)
    if inner is not idl_type:
        inner_type = _read_type(inner, where)
        merged = ExtendedAttributes(annotations.entries + inner_type.extended_attributes.entries)
        return dataclasses.replace(inner_type, nullable=nullable or inner_type.nullable, extended_attributes=merged)

    members: tuple[IdlType, ...] = ()
    name = (
        inner.type_name_without_extended_attributes if hasattr(inner, "type_name_without_extended_attributes") else ""
    )
    if inner.is_any:
        kind, name = TypeKind.ANY, "any"
    elif inner.is_undefined:
        kind, name = TypeKind.UNDEFINED, "undefined"
    elif inner.is_boolean:
        kind, name = TypeKind.BOOLEAN, "boolean"
    elif inner.is_integer:
        kind, name = TypeKind.INTEGER, inner.keyword_typename
    elif inner.is_floating_point_numeric:
        kind, name = TypeKind.FLOATING_POINT, inner.keyword_typename
    elif inner.is_bigint:
        kind, name = TypeKind.BIGINT, "bigint"
    elif inner.is_string:
        kind, name = TypeKind.STRING, inner.keyword_typename
    elif inner.is_object:
        kind, name = TypeKind.OBJECT, "object"
    elif inner.is_symbol:
        kind, name = TypeKind.SYMBOL, "symbol"
    elif inner.is_buffer_source_type:
        kind, name = TypeKind.BUFFER_SOURCE, inner.keyword_typename
    elif inner.is_interface:
        kind, name = TypeKind.INTERFACE, inner.identifier
    elif inner.is_callback_interface:
        kind, name = TypeKind.CALLBACK_INTERFACE, inner.identifier
    elif inner.is_callback_function:
        kind, name = TypeKind.CALLBACK_FUNCTION, inner.identifier
    elif inner.is_sync_iterator or inner.is_async_iterator:
        kind, name = TypeKind.ITERATOR, inner.identifier
    elif inner.is_dictionary:
        kind, name = TypeKind.DICTIONARY, inner.identifier
    elif inner.is_enumeration:
        kind, name = TypeKind.ENUMERATION, inner.identifier
    elif inner.is_sequence:
        kind, name, members = TypeKind.SEQUENCE, "sequence", (_read_type(inner.element_type, where),)
    elif inner.is_frozen_array:
        kind, name, members = TypeKind.FROZEN_ARRAY, "FrozenArray", (_read_type(inner.element_type, where),)
    elif inner.is_observable_array:
        kind, name, members = TypeKind.OBSERVABLE_ARRAY, "ObservableArray", (_read_type(inner.element_type, where),)
    elif inner.is_record:
        kind, name = TypeKind.RECORD, "record"
        members = (_read_type(inner.key_type, where), _read_type(inner.value_type, where))
    elif inner.is_promise:
        kind, name, members = TypeKind.PROMISE, "Promise", (_read_type(inner.result_type, where),)
    elif inner.is_union:
        kind = TypeKind.UNION
        members = tuple(_read_type(member, where) for member in inner.member_types)
        name = " or ".join(member.syntax for member in members)
    else:
        raise UnhandledError(f"{where}: the generator does not handle the type {inner.syntactic_form}")
    return IdlType(kind=kind, name=name, nullable=nullable, extended_attributes=annotations, members=members)


def _read_arguments(arguments: Any, where: str) -> tuple[Argument, ...]:
    result: list[Argument] = []
    for argument in arguments:
        default = argument.default_value
        result.append(
            Argument(
                name=argument.identifier,
                idl_type=_read_type(argument.idl_type, f"argument {argument.identifier} of {where}"),
                is_optional=bool(argument.is_optional),
                is_variadic=bool(argument.is_variadic),
                default=None if default is None else str(default.literal),
            )
        )
    return tuple(result)


class _Exposures:
    """Works out when the members of one interface are exposed, as bind_gen does for a Window."""

    def __init__(self, definition: Any) -> None:
        # The global names that bind_gen counts as implemented: those of a [Global] interface itself, and for any other
        # interface the scopes it is exposed on, when its [Exposed] alone decides them.
        extended = definition.extended_attributes
        self._global_names = list(extended.values_of("Global")) + list(extended.values_of("TargetOfExposed"))
        self._scopes: Any = None
        if not self._global_names and hasattr(definition, "exposure"):
            self._scopes = definition.exposure.unconditional_global_scopes()

    def member_condition(self, member: Any) -> Condition:
        global_names = self._global_names_for_member(member)
        if hasattr(member, "__iter__") and not hasattr(member, "exposure"):
            raise UnhandledError(f"{member.identifier}: an overload group without an exposure")
        return condition_of(member.exposure, global_names)

    def group_condition(self, group: Any) -> Condition:
        return group_condition(group, self._global_names_for_member(group))

    def _global_names_for_member(self, member: Any) -> list[str]:
        if self._scopes is None:
            return self._global_names

        def is_exposed_wherever_class_like_is(exposure: Any) -> bool:
            scopes = exposure.unconditional_global_scopes()
            return scopes is not None and self._scopes <= scopes

        overloads = list(member) if _is_group(member) else [member]
        if not all(is_exposed_wherever_class_like_is(overload.exposure) for overload in overloads):
            return self._global_names
        return sorted({entry.global_name for entry in member.exposure.global_names_and_features})


def group_condition(group: Any, global_names: list[str]) -> Condition:
    """An overload group is exposed where any of its overloads is, as bind_gen's expr_or() of their conditions."""
    conditions = [condition_of(overload.exposure, global_names) for overload in group]
    if any(condition.is_always for condition in conditions):
        return ALWAYS
    if len(set(conditions)) == 1:
        return conditions[0]
    return Condition(
        alternatives=tuple(dict.fromkeys(conditions)),
        context_dependent=bool(group.exposure.is_context_dependent(global_names)),
    )


def _is_group(member: Any) -> bool:
    return hasattr(member, "min_num_of_required_arguments")


def condition_of(exposure: Any, global_names: list[str]) -> Condition:
    """The condition under which bind_gen's expr_from_exposure() installs a construct, beyond its global object.

    Args:
        exposure: the construct's web_idl.Exposure.
        global_names: the global names that the global object implements, as bind_gen counts them.
    """
    features: list[str] = []
    other: list[str] = []
    if exposure.only_in_coi_contexts:
        other.append("CrossOriginIsolated")
    elif exposure.only_in_coi_contexts_or_runtime_enabled_features:
        names = ",".join(exposure.only_in_coi_contexts_or_runtime_enabled_features)
        other.append(f"CrossOriginIsolatedOrRuntimeEnabled={names}")
    if exposure.only_in_injection_mitigated_contexts:
        other.append("InjectionMitigated")
    if exposure.only_in_isolated_contexts:
        other.append("IsolatedContext")
    secure = exposure.only_in_secure_contexts
    if secure is True:
        other.append("SecureContext")
    elif secure:
        other.append(f"SecureContext={','.join(secure)}")
    if global_names:
        exposed_features = [
            entry.feature
            for entry in exposure.global_names_and_features
            if entry.feature and entry.global_name in global_names
        ]
        if exposed_features:
            other.append(f"Exposed with {' or '.join(exposed_features)}")
    else:
        names = [entry.global_name for entry in exposure.global_names_and_features if entry.global_name != "*"]
        if names:
            other.append(f"Exposed={','.join(names)}")
    for feature in exposure.runtime_enabled_features:
        if feature.is_context_dependent:
            other.append(f"RuntimeEnabled={feature} in the context")
        else:
            features.append(str(feature))
    if exposure.context_enabled_features:
        other.append(f"ContextEnabled={','.join(exposure.context_enabled_features)}")
    return Condition(
        features=tuple(features),
        other=tuple(other),
        context_dependent=bool(exposure.is_context_dependent(global_names)),
    )


def _read_attribute(attribute: Any, owner: str, exposures: _Exposures) -> Attribute:
    where = f"attribute {owner}.{attribute.identifier}"
    return Attribute(
        name=attribute.identifier,
        idl_type=_read_type(attribute.idl_type, where),
        is_readonly=bool(attribute.is_readonly),
        is_static=bool(attribute.is_static),
        extended_attributes=_check_extended_attributes(where, attribute),
        condition=exposures.member_condition(attribute),
    )


def _read_operation(operation: Any, owner: str, exposures: _Exposures, kind: str = "operation") -> Operation:
    where = f"{kind} {owner}.{operation.identifier}"
    return_type: Any = getattr(operation, "return_type", None)
    return Operation(
        name=operation.identifier,
        arguments=_read_arguments(operation.arguments, where),
        return_type=IdlType(TypeKind.UNDEFINED, "undefined")
        if return_type is None
        else _read_type(return_type, f"{where} return type"),
        is_static=bool(getattr(operation, "is_static", False)),
        extended_attributes=_check_extended_attributes(where, operation),
        condition=exposures.member_condition(operation),
    )


def _read_group(group: Any, owner: str, exposures: _Exposures, kind: str = "operation") -> OperationGroup:
    where = f"{kind} {owner}.{group.identifier}"
    return OperationGroup(
        name=group.identifier,
        overloads=tuple(_read_operation(overload, owner, exposures, kind) for overload in group),
        is_static=bool(getattr(group, "is_static", False)),
        length=int(group.min_num_of_required_arguments),
        extended_attributes=_check_extended_attributes(where, group),
        condition=exposures.group_condition(group),
    )


def _read_constant(constant: Any, owner: str, exposures: _Exposures) -> Constant:
    where = f"constant {owner}.{constant.identifier}"
    value = constant.value.value
    if not isinstance(value, (int, float, bool)):
        raise UnhandledError(f"{where}: the generator does not handle the value {constant.value.literal}")
    return Constant(
        name=constant.identifier,
        idl_type=_read_type(constant.idl_type, where),
        value=value,
        extended_attributes=_check_extended_attributes(where, constant),
        condition=exposures.member_condition(constant),
    )


def _read_collection(definition: Any, exposures: _Exposures) -> CollectionLike | None:
    owner = definition.identifier
    declarations = [
        ("async iterable", getattr(definition, "async_iterable", None)),
        ("iterable", getattr(definition, "iterable", None)),
        ("maplike", getattr(definition, "maplike", None)),
        ("setlike", getattr(definition, "setlike", None)),
    ]
    present = [(kind, declaration) for kind, declaration in declarations if declaration is not None]
    if not present:
        return None
    if len(present) > 1:
        raise UnhandledError(f"interface {owner}: more than one of iterable, maplike and setlike")
    kind, declaration = present[0]
    where = f"{kind} of {owner}"
    if hasattr(declaration, "extended_attributes"):
        _check_extended_attributes(where, declaration)
    for type_name in ("key_type", "value_type"):
        declared = getattr(declaration, type_name, None)
        if declared is not None:
            _read_type(declared, where)
    for argument in getattr(declaration, "arguments", ()) or ():
        _read_type(argument.idl_type, where)

    # The operations that the interface defines itself replace those that the declaration would add, if Blink marks
    # them as optionally defined.
    defined = {member.identifier for member in itertools.chain(definition.attributes, definition.constants)}
    defined |= {group.identifier for group in definition.operation_groups}
    groups = [
        group
        for group in declaration.operation_groups
        if not group[0].is_optionally_defined or group.identifier not in defined
    ]
    iterator_operation: str | None = None
    if kind == "async iterable":
        iterator_operation = next(
            group.identifier for group in declaration.operation_groups if group[0].is_async_iterator
        )
    elif kind != "iterable" or declaration.key_type is not None:
        iterator_operation = next(group.identifier for group in declaration.operation_groups if group[0].is_iterator)
    return CollectionLike(
        kind=kind,
        attributes=tuple(_read_attribute(attribute, owner, exposures) for attribute in declaration.attributes),
        operation_groups=tuple(_read_group(group, owner, exposures) for group in groups),
        iterator_operation=iterator_operation,
    )


def _read_special_operations(definition: Any, exposures: _Exposures) -> tuple[SpecialOperation, ...]:
    properties: Any = getattr(definition, "indexed_and_named_properties", None)
    if properties is None:
        return ()
    specials: list[SpecialOperation] = []
    for name in ("indexed_getter", "indexed_setter", "named_getter", "named_setter", "named_deleter"):
        operation = getattr(properties, name)
        if operation is not None:
            kind = name.replace("_", " ")
            specials.append(SpecialOperation(kind, _read_operation(operation, definition.identifier, exposures, kind)))
    return tuple(specials)


def _read_exposed(definition: Any) -> tuple[ExposedConstruct, ...]:
    """The properties of a [Global] interface's global object that give the constructs exposed on it, in the order
    that bind_gen's make_property_entries_and_callback_defs() lists them, each with its condition as the global object
    sees it."""
    global_names = list(definition.extended_attributes.values_of("Global"))
    constructs: list[ExposedConstruct] = []
    for construct in definition.exposed_constructs:
        if "LegacyNoInterfaceObject" in construct.extended_attributes:
            continue
        if construct.is_namespace:
            kind = "namespace"
        elif construct.is_callback_interface:
            kind = "callback interface"
        else:
            kind = "interface"
        name = construct.identifier
        constructs.append(ExposedConstruct(name, kind, name, condition_of(construct.exposure, global_names)))
    for alias in definition.legacy_window_aliases:
        where = f"[LegacyWindowAlias={alias.identifier}]"
        _check_extended_attributes(where, alias)
        condition = condition_of(alias.exposure, global_names)
        constructs.append(ExposedConstruct(alias.identifier, "alias", alias.original.identifier, condition))
    for construct in definition.exposed_constructs:
        for group in getattr(construct, "legacy_factory_function_groups", ()):
            constructs.append(
                ExposedConstruct(
                    group.identifier,
                    "factory function",
                    construct.identifier,
                    group_condition(group, global_names),
                    length=int(group.min_num_of_required_arguments),
                )
            )
    return tuple(constructs)


def _read_interface(definition: Any) -> Interface:
    name = definition.identifier
    if definition.is_namespace:
        kind = "namespace"
    elif definition.is_callback_interface:
        kind = "callback interface"
    else:
        kind = "interface"
    exposures = _Exposures(definition)
    extended = _check_extended_attributes(f"{kind} {name}", definition)
    constructor_groups = list(getattr(definition, "constructor_groups", ()))
    if len(constructor_groups) > 1:
        raise UnhandledError(f"interface {name}: more than one group of constructors")
    stringifier: str | None = None
    stringifier_condition = ALWAYS
    if getattr(definition, "stringifier", None) is not None:
        attribute = definition.stringifier.attribute
        stringifier = attribute.identifier if attribute is not None else ""
        stringifier_condition = exposures.member_condition(definition.stringifier.operation)
    inherited: Any = getattr(definition, "inherited", None)
    unscopables: list[str] = []
    if kind == "interface":
        own = [
            m.identifier
            for m in [*definition.attributes, *definition.operations]
            if "Unscopable" in m.extended_attributes
        ]
        if own:
            for ancestor in definition.inclusive_inherited_interfaces:
                members = [*ancestor.attributes, *ancestor.operations]
                unscopables += [m.identifier for m in members if "Unscopable" in m.extended_attributes]
    exposure = getattr(definition, "exposure", None)
    return Interface(
        kind=kind,
        name=name,
        parent=None if inherited is None else inherited.identifier,
        tag=int(getattr(definition, "tag", 0) or 0),
        max_subclass_tag=int(getattr(definition, "max_subclass_tag", 0) or 0),
        exposure=()
        if exposure is None
        else tuple((entry.global_name, entry.feature) for entry in exposure.global_names_and_features),
        condition=ALWAYS if exposure is None else condition_of(exposure, [PAGE_GLOBAL_NAME]),
        extended_attributes=extended,
        attributes=tuple(_read_attribute(attribute, name, exposures) for attribute in definition.attributes),
        constants=tuple(_read_constant(constant, name, exposures) for constant in definition.constants),
        constructor_group=_read_group(constructor_groups[0], name, exposures, "constructor")
        if constructor_groups
        else None,
        legacy_factory_functions=tuple(
            _read_group(group, name, exposures, "legacy factory function")
            for group in getattr(definition, "legacy_factory_function_groups", ())
        ),
        operation_groups=tuple(_read_group(group, name, exposures) for group in definition.operation_groups),
        stringifier=stringifier,
        collection=_read_collection(definition, exposures) if kind == "interface" else None,
        special_operations=_read_special_operations(definition, exposures) if kind == "interface" else (),
        legacy_window_aliases=tuple(alias.identifier for alias in getattr(definition, "legacy_window_aliases", ())),
        exposed_constructs=tuple(construct.identifier for construct in getattr(definition, "exposed_constructs", ())),
        unscopables=tuple(sorted(unscopables)),
        stringifier_condition=stringifier_condition,
        exposed=_read_exposed(definition) if "Global" in extended else (),
    )


def _callback_interface(interface: Interface) -> CallbackInterface:
    operations = [group.name for group in interface.operation_groups if not group.is_static]
    if len(operations) != 1:
        raise UnhandledError(f"callback interface {interface.name}: the generator handles one operation only")
    return CallbackInterface(name=interface.name, operation=operations[0])


def _read_dictionary(definition: Any) -> Dictionary:
    name = definition.identifier
    extended = _check_extended_attributes(f"dictionary {name}", definition)
    members: list[DictionaryMember] = []
    for member in definition.own_members:
        where = f"dictionary member {name}.{member.identifier}"
        default = member.default_value
        members.append(
            DictionaryMember(
                name=member.identifier,
                idl_type=_read_type(member.idl_type, where),
                is_required=bool(member.is_required),
                default=None if default is None else str(default.literal),
                extended_attributes=_check_extended_attributes(where, member),
                condition=condition_of(member.exposure, []),
            )
        )
    inherited: Any = definition.inherited
    return Dictionary(
        name=name,
        parent=None if inherited is None else inherited.identifier,
        own_members=tuple(members),
        extended_attributes=extended,
    )
