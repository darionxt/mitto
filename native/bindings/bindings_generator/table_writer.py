"""Writes the tables of the interfaces, namespaces and callback interfaces that have no C++ class.

Every construct that a Page's global object exposes, and every ancestor of one, gets bindings, as ADR-0008 has it. One
that has a C++ class gets the code of cpp_writer.py. Every other gets a table of its own properties in the order of its
install plan, which native/bindings/interface_table.cc installs at run time, with a WrapperTypeInfo that points at it.
A table costs a few bytes for each member, where code would cost a function, so a thousand interfaces stay small in the
Page program and quick to build.

The tables go into a fixed number of files, so that GN knows them before the generator runs and compiles them side by
side, and one header declares the WrapperTypeInfo of each construct, which the bindings of Window name for its exposed
constructs.
"""

# Upstream: third_party/blink/renderer/bindings/scripts/bind_gen/interface.py

import dataclasses
from collections.abc import Callable

from bindings_generator.cpp_types import header_of
from bindings_generator.errors import UnhandledError
from bindings_generator.install_plan import Phase, Property, plan_interface
from bindings_generator.known_gaps import KnownGaps, default_tojson_attributes, has_default_tojson
from bindings_generator.model import (
    ALWAYS,
    Attribute,
    Condition,
    Constant,
    Dictionary,
    ExtendedAttributes,
    IdlType,
    Interface,
    Operation,
    OperationGroup,
    TypeKind,
)
from bindings_generator.type_tables import HEADER as TYPES_HEADER
from bindings_generator.type_tables import ConditionFunctions, TypeTables

__all__ = ["HEADER", "Construct", "KnownGapContext", "member_row", "shard_name", "write_tables"]

HEADER = "interfaces_without_class.h"

_BANNER = "// Written by the bindings generator in native/bindings from Blink's Web IDL\n// database. Do not edit.\n"

# The value iterator's methods, which are Array.prototype's own (Web IDL's "define the iteration methods").
_ARRAY_INTRINSICS = {
    "entries": "v8::kArrayProto_entries",
    "keys": "v8::kArrayProto_keys",
    "values": "v8::kArrayProto_values",
    "forEach": "v8::kArrayProto_forEach",
}

_TARGETS = {"interface": "kInterface", "prototype": "kPrototype", "instance": "kInstance"}

_KINDS = {
    "sync iterator": "WrapperTypeInfo::Kind::kIterator",
    "async iterator": "WrapperTypeInfo::Kind::kIterator",
    "interface": "WrapperTypeInfo::Kind::kInterface",
    "callback interface": "WrapperTypeInfo::Kind::kCallbackInterface",
    "namespace": "WrapperTypeInfo::Kind::kNamespace",
}


@dataclasses.dataclass(frozen=True)
class Construct:
    """A construct that gets a table, with what its WrapperTypeInfo needs from the others.

    Attributes:
        interface: the construct.
        index: its WrapperTypeInfo::index.
        interface_object_parent: the nearest ancestor that has an interface object, or None.
        in_global_prototype_chain: whether its prototype is on a global object's chain.
    """

    interface: Interface
    index: int
    interface_object_parent: str | None
    in_global_prototype_chain: bool
    display_name: str | None = None
    """The name a page sees, where it is not the identifier: "Headers Iterator" for SyncIterator_Headers."""
    parent_intrinsic: str | None = None
    """For an iterator, the intrinsic its prototype inherits from, such as "v8::kIteratorPrototype"."""


def shard_name(shard: int) -> str:
    """The file that holds the tables of one shard."""
    return f"interfaces_without_class_{shard}.cc"


def _quote(text: str) -> str:
    return '"' + text + '"'


def _attributes(property_: Property) -> str:
    flags: list[str] = []
    if not property_.enumerable:
        flags.append("v8::DontEnum")
    if not property_.configurable:
        flags.append("v8::DontDelete")
    if property_.writable is False and property_.kind != "accessor":
        flags.append("v8::ReadOnly")
    return " | ".join(flags) if flags else "v8::None"


@dataclasses.dataclass(frozen=True)
class KnownGapContext:
    """What the rows of Known Gaps name: the tables of types and operations, and the rules of Known Gaps."""

    types: TypeTables
    gaps: KnownGaps
    factory_functions: dict[str, OperationGroup] = dataclasses.field(default_factory=dict[str, OperationGroup])
    """Each [LegacyFactoryFunction], by its name."""
    marks: dict[tuple[str, str, str], str] = dataclasses.field(default_factory=dict[tuple[str, str, str], str])
    """What the listing says of each property that has no behaviour, by its interface, target and name."""
    interfaces: dict[str, Interface] = dataclasses.field(default_factory=dict[str, Interface])
    """Every interface, for the attributes that a toJSON() reads."""
    dictionaries: dict[str, Dictionary] = dataclasses.field(default_factory=dict[str, Dictionary])


def member_row(
    owner: Interface,
    property_: Property,
    context: KnownGapContext,
    condition: str = "nullptr",
) -> str | None:
    """The MemberInfo of one property of an interface, or None for one that Chrome's plan holds and the table leaves to
    V8 or to the template's set-up.

    Args:
        owner: the interface the property belongs to.
        property_: the property, from the install plan.
        context: the tables and rules of Known Gaps.
        condition: the address of the function that says whether a Page installs it.
    """
    where = f"{owner.name}.{property_.name}"
    if property_.name in ("@@toStringTag", "constructor"):
        return None
    member = property_.member
    kind = "kAttribute"
    flags: list[str] = []
    value = "0"
    forward = "nullptr"
    type_info = "nullptr"
    operation = "nullptr"
    number = "0"
    length = property_.length or 0
    if property_.name == "@@unscopables":
        kind = "kUnscopables"
    elif property_.name in ("@@iterator", "@@asyncIterator") and property_.phase is Phase.CONTEXT:
        collection = owner.collection
        if collection is None or collection.iterator_operation is None:
            raise UnhandledError(f"{where}: an iterator symbol without an iterator operation")
        kind, forward = "kIteratorAlias", _quote(collection.iterator_operation)
    elif property_.name == "@@iterator":
        kind, value = "kIntrinsic", "v8::kArrayProto_values"
    elif member is None and property_.name in _ARRAY_INTRINSICS and property_.kind == "method":
        kind, value = "kIntrinsic", _ARRAY_INTRINSICS[property_.name]
    elif member is None and property_.name == "toString":
        # The stringifier, an operation of its own, which gives a string.
        kind = "kOperation"
        operation = context.types.operation(_STRINGIFIER, where)
        attribute = next((each for each in owner.attributes if each.name == owner.stringifier), None)
        if attribute is None:
            number = str(context.gaps.number())
        else:
            # Blink's toString() of a stringifier attribute is the attribute's getter, so it gives what the getter
            # gives, the value that the setter stored among it.
            flags.append("kMemberStringifierAttribute")
            forward = _quote(attribute.name)
            number = str(context.gaps.attribute_number(owner.name, attribute.name))
            type_info = context.types.type(attribute.idl_type, f"attribute {owner.name}.{attribute.name}")
            if context.gaps.refuses(owner, attribute):
                flags.append("kMemberRefused")
    elif isinstance(member, Attribute):
        number = str(context.gaps.attribute_number(owner.name, member.name))
        type_info = context.types.type(member.idl_type, f"attribute {where}")
        if property_.setter:
            flags.append("kMemberHasSetter")
        extended = member.extended_attributes
        if "LegacyLenientThis" in extended:
            flags.append("kMemberLenientThis")
        if "Replaceable" in extended:
            flags.append("kMemberReplaceable")
        elif "LegacyLenientSetter" in extended:
            flags.append("kMemberLenientSetter")
        elif "PutForwards" in extended:
            flags.append("kMemberPutForwards")
            forward = _quote(extended.value_of("PutForwards") or "")
        if member.is_static:
            flags.append("kMemberStatic")
        if "NewObject" in extended:
            flags.append("kMemberNewObject")
        if context.gaps.is_event_handler(member):
            flags.append("kMemberEventHandler")
        elif context.gaps.refuses(owner, member):
            flags.append("kMemberRefused")
    elif isinstance(member, OperationGroup) and owner.kind in ("sync iterator", "async iterator"):
        # An iterator over nothing: next() is done at once, and return() too.
        kind = "kOperation"
        flags.append("kMemberIteratorReturn" if member.name == "return" else "kMemberIteratorNext")
        if owner.kind == "async iterator":
            flags.append("kMemberReturnsPromise")
    elif isinstance(member, OperationGroup):
        kind, number = "kOperation", str(context.gaps.number())
        operation = context.types.operation(member, f"operation {where}")
        if has_default_tojson(member):
            # The default toJSON steps, as the owner chose on 2026-11-12: the attributes they read, by their index.
            flags.append("kMemberDefaultToJSON")
            attributes = default_tojson_attributes(owner, context.interfaces, context.dictionaries)
            value = str(context.types.json_attributes(attributes, f"operation {where}"))
        if member.is_static:
            flags.append("kMemberStatic")
        if any(overload.return_type.kind is TypeKind.PROMISE for overload in member.overloads):
            flags.append("kMemberReturnsPromise")
        if context.gaps.refuses(owner, member):
            flags.append("kMemberRefused")
    elif isinstance(member, Constant):
        if isinstance(member.value, bool) or not isinstance(member.value, int):
            raise UnhandledError(f"constant {where}: the generator handles integer constants only")
        kind, value = "kConstant", str(member.value)
    else:
        raise UnhandledError(f"{where}: the table writer does not install this kind of property")
    if kind in ("kAttribute", "kOperation"):
        mark = "Known Gap"
        if "kMemberEventHandler" in flags:
            mark = "event handler"
        elif "kMemberRefused" in flags:
            mark = "Known Gap, refused"
        context.marks[(owner.name, property_.target, property_.name)] = mark
    return (
        f"    {{{_quote(property_.name)}, &V8{owner.name}::kWrapperTypeInfo, {condition}, {forward}, {type_info}, "
        f"{operation}, {value}, {' | '.join(flags) if flags else '0'}, {number}, MemberKind::{kind}, "
        f"MemberTarget::{_TARGETS[property_.target]}, {_attributes(property_)}, {length}}},"
    )


# A stringifier as an operation: no arguments, and a string.
_STRINGIFIER = OperationGroup(
    name="toString",
    overloads=(Operation("toString", (), IdlType(TypeKind.STRING, "DOMString"), False, ExtendedAttributes(), ALWAYS),),
    is_static=False,
    length=0,
    extended_attributes=ExtendedAttributes(),
    condition=ALWAYS,
)


class _ShardWriter:
    """Writes one file of tables, with the condition functions its members share."""

    def __init__(self, condition_test: Callable[[Condition, str], str | None], context: KnownGapContext) -> None:
        self._conditions = ConditionFunctions(condition_test)
        self._context = context
        self.tables: list[str] = []
        self.parents: set[str] = set()

    @property
    def functions(self) -> list[str]:
        return self._conditions.lines

    def condition(self, condition: Condition, where: str) -> str | None:
        """The condition function of a construct, "nullptr" for always, or None for never."""
        return self._conditions.reference(condition, where)

    def member(self, interface: Interface, property_: Property) -> str | None:
        """The MemberInfo of one property, or None for one that the table leaves out or that a Page never installs."""
        condition = "nullptr"
        if property_.member is not None:
            found = self.condition(property_.condition, f"{interface.name}.{property_.name}")
            if found is None:
                return None
            condition = found
        return member_row(interface, property_, self._context, condition)

    def construct(self, construct: Construct) -> None:
        interface = construct.interface
        name = interface.name
        phases: dict[Phase, list[str]] = {Phase.TEMPLATE: [], Phase.CONTEXT: []}
        for property_ in plan_interface(interface):
            if property_.target == "instance" and property_.phase is Phase.CONTEXT:
                raise UnhandledError(f"{name}.{property_.name}: a member of each instance that each context decides")
            row = self.member(interface, property_)
            if row is not None:
                phases[property_.phase].append(row)
        arrays: dict[Phase, str] = {}
        for phase, rows in phases.items():
            if rows:
                array = f"k{name}{phase.value.capitalize()}Members"
                self.tables += [f"constexpr MemberInfo {array}[] = {{", *rows, "};", ""]
                arrays[phase] = array
        unscopables = "{}"
        if interface.unscopables:
            unscopables = f"k{name}Unscopables"
            names = ", ".join(_quote(item) for item in interface.unscopables)
            self.tables += [f"constexpr const char* {unscopables}[] = {{{names}}};", ""]
        constructor, constructor_condition, length = "kNone", "nullptr", 0
        constructor_operation, constructor_number = "nullptr", "0"
        flags: list[str] = []
        group = interface.constructor_group
        if group is not None:
            found = self.condition(dataclasses.replace(group.condition, context_dependent=False), f"{name} constructor")
            if group.condition.context_dependent:
                # bind_gen installs a constructor that each context decides on the template, whatever the context.
                found = "nullptr"
            if found is not None:
                html = any("HTMLConstructor" in overload.extended_attributes for overload in group.overloads)
                constructor = "kHTMLConstructor" if html else "kKnownGap"
                constructor_condition, length = found, group.length
                if not html:
                    constructor_operation = self._context.types.operation(group, f"constructor of {name}")
                    constructor_number = str(self._context.gaps.number())
                    if self._context.gaps.refuses(interface, None):
                        flags.append("kInterfaceConstructorRefused")
        behind = self._context.gaps.class_behind(name)
        object_behind = {None: "kNone", "EventTarget": "kEventTarget", "Event": "kEvent"}.get(behind, "kNone")
        if construct.in_global_prototype_chain:
            flags.append("kInterfaceInGlobalPrototypeChain")
        if name == "Location":
            flags.append("kInterfaceLocation")
        if name == "HTMLAllCollection":
            flags.append("kInterfaceAllCollection")
        if "IsCodeLike" in interface.extended_attributes:
            flags.append("kInterfaceCodeLike")
        if "LegacyNoInterfaceObject" in interface.extended_attributes or construct.parent_intrinsic:
            # An iterator's prototype has no `constructor` either.
            flags.append("kInterfaceNoInterfaceObject")
        self.tables += [
            f"constexpr InterfaceTable k{name}Table = {{",
            f"    {arrays.get(Phase.TEMPLATE, '{}')},",
            f"    {arrays.get(Phase.CONTEXT, '{}')},",
            f"    {unscopables},",
            f"    {constructor_condition},",
            f"    {constructor_operation},",
            f"    ConstructorKind::{constructor},",
            f"    ObjectBehind::{object_behind},",
            f"    {construct.parent_intrinsic or '-1'},",
            f"    {length},",
            f"    {constructor_number},",
            f"    {' | '.join(flags) if flags else '0'},",
            "};",
            "",
        ]
        for parent in (interface.parent, construct.interface_object_parent):
            if parent is not None:
                self.parents.add(parent)

    def wrapper_type_info(self, construct: Construct) -> list[str]:
        interface = construct.interface
        parent = "nullptr" if interface.parent is None else f"&V8{interface.parent}::kWrapperTypeInfo"
        object_parent = construct.interface_object_parent
        object_parent_info = "nullptr" if object_parent is None else f"&V8{object_parent}::kWrapperTypeInfo"
        return [
            f"const WrapperTypeInfo V8{interface.name}::kWrapperTypeInfo = {{",
            f"    {{{interface.tag}}},",
            f"    {_quote(construct.display_name or interface.name)},",
            f"    {parent},",
            f"    static_cast<v8::CppHeapPointerTag>({interface.tag}),",
            f"    static_cast<v8::CppHeapPointerTag>({interface.max_subclass_tag}),",
            "    &InstallTemplateFromTable,",
            f"    {object_parent_info},",
            "    &InstallContextPropertiesFromTable,",
            f"    {_KINDS[interface.kind]},",
            f"    {construct.index},",
            f"    &k{interface.name}Table,",
            "};",
            "",
        ]


def _header(constructs: list[Construct]) -> str:
    lines = [
        _BANNER,
        "#ifndef MITTO_BINDINGS_INTERFACES_WITHOUT_CLASS_H_",
        "#define MITTO_BINDINGS_INTERFACES_WITHOUT_CLASS_H_",
        "",
        '#include "mitto/bindings/wrapper_type_info.h"',
        "",
        "namespace mitto::bindings {",
        "",
        "// The interfaces, namespaces and callback interfaces that a page can reach",
        "// and that have no C++ class, whose members are in tables",
        "// (interface_table.h).",
    ]
    for construct in constructs:
        lines += [
            f"class V8{construct.interface.name} final {{",
            " public:",
            "  static const WrapperTypeInfo kWrapperTypeInfo;",
            "};",
        ]
    lines += ["", "}  // namespace mitto::bindings", "", "#endif  // MITTO_BINDINGS_INTERFACES_WITHOUT_CLASS_H_", ""]
    return "\n".join(lines)


def _split(constructs: list[Construct], shards: int) -> list[list[Construct]]:
    """The constructs in `shards` runs of about as many members each, in order, so that the files take about as long
    to compile."""
    weights = [1 + len(plan_interface(construct.interface)) for construct in constructs]
    total = sum(weights)
    parts: list[list[Construct]] = [[] for _ in range(shards)]
    done = 0
    for construct, weight in zip(constructs, weights, strict=True):
        parts[min(shards - 1, done * shards // max(total, 1))].append(construct)
        done += weight
    return parts


def write_tables(
    constructs: list[Construct],
    shards: int,
    has_class: Callable[[str], bool],
    condition_test: Callable[[Condition, str], str | None],
    context: KnownGapContext,
) -> dict[str, str]:
    """Returns the header and the files of the tables, by their names in the output directory.

    Args:
        constructs: the constructs that get tables, sorted by name.
        shards: how many files of tables to write.
        has_class: whether an interface has a C++ class, whose bindings' header names its WrapperTypeInfo.
        condition_test: the C++ test of a condition, as cpp_writer.condition_test() writes it.
        context: the tables of types and operations, and the rules of Known Gaps, which the tables' rows name.
    """
    files = {HEADER: _header(constructs)}
    for shard, part in enumerate(_split(constructs, shards)):
        writer = _ShardWriter(condition_test, context)
        definitions: list[str] = []
        for construct in part:
            writer.construct(construct)
            definitions += writer.wrapper_type_info(construct)
        includes = {
            f"mitto/bindings/{HEADER}",
            f"mitto/bindings/{TYPES_HEADER}",
            "mitto/bindings/interface_table.h",
            "mitto/bindings/per_isolate_data.h",
            "mitto/bindings/runtime_feature_set.h",
            "mitto/bindings/runtime_features.h",
        }
        includes |= {header_of(parent) for parent in writer.parents if has_class(parent)}
        lines = [
            _BANNER,
            "// The tables of these constructs, which interface_table.cc installs:",
            *[f"// {construct.interface.name}" for construct in part],
            "",
            *[f'#include "{include}"' for include in sorted(includes)],
            '#include "v8/include/v8-object.h"',
            '#include "v8/include/v8-template.h"',
            "",
            "namespace mitto::bindings {",
            "",
            "namespace {",
            "",
            *writer.functions,
            *writer.tables,
            "}  // namespace",
            "",
            *definitions,
            "}  // namespace mitto::bindings",
            "",
        ]
        files[shard_name(shard)] = "\n".join(lines)
    return files
