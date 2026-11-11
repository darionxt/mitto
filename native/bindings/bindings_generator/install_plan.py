"""The properties that Chrome installs for an interface, in the order it installs them.

Blink's bind_gen writes, for each interface, functions that install its members: first on V8's templates, those that
are always there and then those behind a runtime-enabled feature, and later on each context's objects, those that the
context decides. V8 adds `constructor` to the prototype when it makes the prototype from its template, so it comes
between the two. Within each function, Blink installs the attributes, then the constants, the exposed constructs and
the operations, with those of one condition together. The plan here follows that, so that a page sees each object's
own properties in Chrome's order.
"""

# Upstream: third_party/blink/renderer/bindings/scripts/bind_gen/interface.py
# Upstream: third_party/blink/renderer/platform/bindings/idl_member_installer.cc
# Upstream: third_party/blink/renderer/bindings/core/v8/generated_code_helper.cc

import dataclasses
import enum

from bindings_generator.model import ALWAYS, Attribute, Condition, Constant, Interface, OperationGroup

__all__ = ["Phase", "Property", "plan_interface"]


class Phase(enum.Enum):
    TEMPLATE = "template"
    """On V8's templates, when a Page builds them, before V8 adds `constructor` to the prototype."""
    CONTEXT = "context"
    """On the objects of each context, after V8 made them from the templates."""


Member = Attribute | Constant | OperationGroup | None


@dataclasses.dataclass(frozen=True)
class Property:
    """One own property of an interface object, prototype object or instance.

    Attributes:
        name: the property's name, or for a symbol its well-known name, such as "@@iterator".
        target: "interface", "prototype" or "instance".
        kind: "accessor", "method" or "value".
        enumerable, configurable: its attributes.
        writable: whether a value can change, None for an accessor.
        length: a method's length.
        getter, setter: the names of an accessor's functions, such as "get type", or None.
        value: a constant's value, or the string a data property holds.
        member: what in the model the property comes from, None for what Blink adds of its own.
        condition: when it is installed.
        phase: when Chrome installs it.
        no_alloc_direct_call: whether Blink installs it with V8's fast calls, after the others of its kind.
    """

    name: str
    target: str
    kind: str
    enumerable: bool
    configurable: bool
    writable: bool | None
    length: int | None = None
    getter: str | None = None
    setter: str | None = None
    value: int | float | bool | str | None = None
    member: Member = None
    condition: Condition = ALWAYS
    phase: Phase = Phase.TEMPLATE
    no_alloc_direct_call: bool = False


@dataclasses.dataclass(frozen=True)
class _Entry:
    """A member as bind_gen's make_property_entries_and_callback_defs() lists it."""

    group: str
    """"attribute", "constant" or "operation"."""
    properties: tuple[Property, ...]
    condition: Condition
    no_alloc_direct_call: bool


def _attribute_entry(attribute: Attribute, interface: Interface) -> _Entry:
    extended = attribute.extended_attributes
    unforgeable = "LegacyUnforgeable" in extended
    if attribute.is_static:
        target = "interface"
    elif "Global" in interface.extended_attributes or unforgeable:
        target = "instance"
    else:
        target = "prototype"
    has_setter = not attribute.is_readonly or any(
        key in extended for key in ("LegacyLenientSetter", "PutForwards", "Replaceable")
    )
    nadc = "NoAllocDirectCall" in extended and "Setter" in extended.values_of("NoAllocDirectCall")
    property_ = Property(
        name=attribute.name,
        target=target,
        kind="accessor",
        enumerable="NotEnumerable" not in extended,
        configurable=not unforgeable,
        writable=None,
        getter=f"get {attribute.name}",
        setter=f"set {attribute.name}" if has_setter else None,
        member=attribute,
        condition=attribute.condition,
        no_alloc_direct_call=nadc,
    )
    return _Entry("attribute", (property_,), attribute.condition, nadc)


def _constant_entry(constant: Constant) -> _Entry:
    # IDLMemberInstaller::InstallConstants() puts each constant on the prototype and then on the interface object.
    properties = tuple(
        Property(
            name=constant.name,
            target=target,
            kind="value",
            enumerable=True,
            configurable=False,
            writable=False,
            value=constant.value,
            member=constant,
            condition=constant.condition,
        )
        for target in ("prototype", "interface")
    )
    return _Entry("constant", properties, constant.condition, False)


def _operation_entry(group: OperationGroup, interface: Interface, name: str | None = None) -> _Entry:
    extended = group.extended_attributes
    unforgeable = "LegacyUnforgeable" in extended
    if group.is_static:
        target = "interface"
    elif "Global" in interface.extended_attributes or unforgeable:
        target = "instance"
    else:
        target = "prototype"
    nadc = "NoAllocDirectCall" in extended
    property_ = Property(
        name=name or group.name,
        target=target,
        kind="method",
        enumerable="NotEnumerable" not in extended,
        configurable=not unforgeable,
        writable=not unforgeable,
        length=group.length,
        member=group,
        condition=group.condition,
        no_alloc_direct_call=nadc,
    )
    return _Entry("operation", (property_,), group.condition, nadc)


def _stringifier_entry(interface: Interface) -> _Entry:
    property_ = Property(
        name="toString",
        target="instance" if "Global" in interface.extended_attributes else "prototype",
        kind="method",
        enumerable=True,
        configurable=True,
        writable=True,
        length=0,
        condition=interface.stringifier_condition,
    )
    return _Entry("operation", (property_,), interface.stringifier_condition, False)


def _entries(interface: Interface) -> list[_Entry]:
    """The members in the order that make_property_entries_and_callback_defs() lists them."""
    entries = [_attribute_entry(attribute, interface) for attribute in interface.attributes]
    entries += [_constant_entry(constant) for constant in interface.constants]
    if interface.kind != "callback interface":
        entries += [_operation_entry(group, interface) for group in interface.operation_groups]
    if interface.stringifier is not None:
        entries.append(_stringifier_entry(interface))
    if interface.collection is not None:
        entries += [_attribute_entry(attribute, interface) for attribute in interface.collection.attributes]
        entries += [_operation_entry(group, interface) for group in interface.collection.operation_groups]
    return entries


def _install_function(entries: list[_Entry], phase: Phase) -> list[Property]:
    """The properties that one of bind_gen's install functions installs, in its order: see make_install_properties()."""
    result: list[Property] = []
    for group, nadc in (("attribute", False), ("attribute", True), ("constant", False), ("operation", False), (
        "operation",
        True,
    )):
        selected = [entry for entry in entries if entry.group == group and entry.no_alloc_direct_call == nadc]
        by_condition: dict[Condition, list[_Entry]] = {}
        for entry in selected:
            by_condition.setdefault(entry.condition, []).append(entry)
        ordered = by_condition.pop(ALWAYS, [])
        for condition_entries in by_condition.values():
            ordered += condition_entries
        for entry in ordered:
            result += [dataclasses.replace(property_, phase=phase) for property_ in entry.properties]
    return result


def _symbol(name: str, phase: Phase, enumerable: bool = False, value: str | None = None) -> Property:
    return Property(
        name=name, target="prototype", kind="value", enumerable=enumerable, configurable=True, writable=False, value=value,
        phase=phase,
    )


def plan_interface(interface: Interface) -> list[Property]:
    """The own properties of the interface's objects, in the order Chrome installs them.

    The interface object's own `length`, `name` and `prototype`, and the prototype's `constructor`, are V8's: the plan
    names `constructor` where V8 adds it, and the others not.
    """
    properties: list[Property] = []
    if interface.kind == "interface":
        # SetupIDLInterfaceTemplate() gives the prototype its @@toStringTag before anything else.
        properties.append(_symbol("@@toStringTag", Phase.TEMPLATE, value=interface.name))
        if "indexed getter" in interface.special_operations and "Global" not in interface.extended_attributes:
            properties.append(_symbol("@@iterator", Phase.TEMPLATE))
        collection = interface.collection
        if collection is not None and collection.kind == "iterable" and collection.iterator_operation is None:
            for name in ("entries", "keys", "values", "forEach"):
                properties.append(
                    Property(name=name, target="prototype", kind="method", enumerable=True, configurable=True,
                             writable=True, phase=Phase.TEMPLATE)
                )
    elif interface.kind == "namespace":
        properties.append(
            Property(name="@@toStringTag", target="interface", kind="value", enumerable=False, configurable=True,
                     writable=False, value=interface.name)
        )

    entries = _entries(interface)
    unconditional = [entry for entry in entries if entry.condition.is_always]
    independent = [e for e in entries if not e.condition.is_always and not e.condition.context_dependent]
    dependent = [entry for entry in entries if entry.condition.context_dependent]
    properties += _install_function(unconditional, Phase.TEMPLATE)
    properties += _install_function(independent, Phase.TEMPLATE)

    if interface.kind == "interface":
        properties.append(
            Property(name="constructor", target="prototype", kind="value", enumerable=False, configurable=True,
                     writable=True, phase=Phase.TEMPLATE)
        )
        # The context's part, which _make_install_prototype_object() starts with.
        if interface.unscopables:
            properties.append(_symbol("@@unscopables", Phase.CONTEXT))
        collection = interface.collection
        if collection is not None and collection.iterator_operation is not None:
            symbol = "@@asyncIterator" if collection.kind == "async iterable" else "@@iterator"
            properties.append(_symbol(symbol, Phase.CONTEXT))
    properties += _install_function(dependent, Phase.CONTEXT)
    if "LegacyNoInterfaceObject" in interface.extended_attributes:
        properties = [p for p in properties if not (p.name == "constructor" and p.target == "prototype")]
    return properties
