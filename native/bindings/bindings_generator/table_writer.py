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
from bindings_generator.model import Attribute, Condition, Constant, Interface, OperationGroup, TypeKind

__all__ = ["HEADER", "Construct", "shard_name", "write_tables"]

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


class _ShardWriter:
    """Writes one file of tables, with the condition functions its members share."""

    def __init__(self, condition_test: Callable[[Condition, str], str | None]) -> None:
        self._condition_test = condition_test
        self._conditions: dict[str, str] = {}
        self.functions: list[str] = []
        self.tables: list[str] = []
        self.parents: set[str] = set()

    def condition(self, condition: Condition, where: str) -> str | None:
        """The condition function of a construct, "nullptr" for always, or None for never."""
        test = self._condition_test(condition, where)
        if test is None:
            return None
        if test == "true":
            return "nullptr"
        if test not in self._conditions:
            name = f"&Condition{len(self._conditions)}"
            self._conditions[test] = name
            self.functions += [
                f"bool Condition{len(self._conditions) - 1}(const PerIsolateData& isolate_data) {{",
                "  [[maybe_unused]] const PerIsolateData* const data = &isolate_data;",
                "  [[maybe_unused]] const RuntimeFeatureSet& features = isolate_data.features();",
                f"  return {test};",
                "}",
                "",
            ]
        return self._conditions[test]

    def member(self, interface: Interface, property_: Property) -> str | None:
        """The MemberInfo of one property, or None for one that Chrome's plan holds and the table leaves to V8, or to
        the template's set-up, or that a Page never installs."""
        where = f"{interface.name}.{property_.name}"
        if property_.name in ("@@toStringTag", "constructor"):
            return None
        member = property_.member
        kind = "kAttribute"
        flags: list[str] = []
        value = "0"
        forward = "nullptr"
        length = property_.length or 0
        if property_.name == "@@unscopables":
            kind = "kUnscopables"
        elif property_.name in ("@@iterator", "@@asyncIterator") and property_.phase is Phase.CONTEXT:
            collection = interface.collection
            if collection is None or collection.iterator_operation is None:
                raise UnhandledError(f"{where}: an iterator symbol without an iterator operation")
            kind, forward = "kIteratorAlias", _quote(collection.iterator_operation)
        elif property_.name == "@@iterator":
            kind, value = "kIntrinsic", "v8::kArrayProto_values"
        elif member is None and property_.name in _ARRAY_INTRINSICS and property_.kind == "method":
            kind, value = "kIntrinsic", _ARRAY_INTRINSICS[property_.name]
        elif member is None and property_.name == "toString":
            # The stringifier, an operation of its own.
            kind = "kOperation"
        elif isinstance(member, Attribute):
            if property_.setter:
                flags.append("kMemberHasSetter")
            extended = member.extended_attributes
            if "LegacyLenientThis" in extended:
                flags.append("kMemberLenientThis")
            if "Replaceable" in extended:
                flags.append("kMemberReplaceable")
            elif "LegacyLenientSetter" in extended:
                flags.append("kMemberLenientSetter")
            if member.is_static:
                flags.append("kMemberStatic")
        elif isinstance(member, OperationGroup):
            kind = "kOperation"
            if member.is_static:
                flags.append("kMemberStatic")
            if any(overload.return_type.kind is TypeKind.PROMISE for overload in member.overloads):
                flags.append("kMemberReturnsPromise")
        elif isinstance(member, Constant):
            if isinstance(member.value, bool) or not isinstance(member.value, int):
                raise UnhandledError(f"constant {where}: the generator handles integer constants only")
            kind, value = "kConstant", str(member.value)
        else:
            raise UnhandledError(f"{where}: the table writer does not install this kind of property")
        condition = "nullptr"
        if member is not None:
            found = self.condition(property_.condition, where)
            if found is None:
                return None
            condition = found
        return (
            f"    {{{_quote(property_.name)}, &V8{interface.name}::kWrapperTypeInfo, {condition}, {forward}, {value}, "
            f"MemberKind::{kind}, MemberTarget::{_TARGETS[property_.target]}, {_attributes(property_)}, "
            f"{' | '.join(flags) if flags else '0'}, {length}}},"
        )

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
        flags: list[str] = []
        if construct.in_global_prototype_chain:
            flags.append("kInterfaceInGlobalPrototypeChain")
        if name == "Location":
            flags.append("kInterfaceLocation")
        if name == "HTMLAllCollection":
            flags.append("kInterfaceAllCollection")
        if "IsCodeLike" in interface.extended_attributes:
            flags.append("kInterfaceCodeLike")
        if "LegacyNoInterfaceObject" in interface.extended_attributes:
            flags.append("kInterfaceNoInterfaceObject")
        self.tables += [
            f"constexpr InterfaceTable k{name}Table = {{",
            f"    {arrays.get(Phase.TEMPLATE, '{}')},",
            f"    {arrays.get(Phase.CONTEXT, '{}')},",
            f"    {unscopables},",
            f"    {constructor_condition},",
            f"    ConstructorKind::{constructor},",
            f"    {length},",
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
            f"    {_quote(interface.name)},",
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
) -> dict[str, str]:
    """Returns the header and the files of the tables, by their names in the output directory.

    Args:
        constructs: the constructs that get tables, sorted by name.
        shards: how many files of tables to write.
        has_class: whether an interface has a C++ class, whose bindings' header names its WrapperTypeInfo.
        condition_test: the C++ test of a condition, as cpp_writer.condition_test() writes it.
    """
    files = {HEADER: _header(constructs)}
    for shard, part in enumerate(_split(constructs, shards)):
        writer = _ShardWriter(condition_test)
        definitions: list[str] = []
        for construct in part:
            writer.construct(construct)
            definitions += writer.wrapper_type_info(construct)
        includes = {
            f"mitto/bindings/{HEADER}",
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
