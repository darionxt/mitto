"""Every extended attribute the generator knows, and what it does with each.

The generator refuses any other, naming it, so that an extended attribute new in an Engine Release's Web IDL files
stops the build until someone decides what it means for a page. Blink documents them in
third_party/blink/renderer/bindings/IDLExtendedAttributes.md.

Each one has a role:

- PLAN: decides which properties exist, where and with which attributes. The install plan follows it for every
  interface.
- EXPOSURE: decides when a construct is exposed. The install plan follows it for every interface.
- CONVERSION: decides how a value converts between JavaScript and C++.
- BEHAVIOUR: decides something a member does beyond converting its arguments and calling its class.
- NONE: only says how Blink implements a member, such as which C++ name or arguments it calls, or what it counts.
  Nothing of it reaches a page.

The C++ writer handles a subset of the first four (see cpp_writer.WRITTEN), and refuses the others on an interface it
writes.
"""

# Upstream: third_party/blink/renderer/bindings/IDLExtendedAttributes.md

import enum

__all__ = ["KNOWN", "Role", "check"]

from bindings_generator.errors import UnhandledError


class Role(enum.Enum):
    PLAN = "plan"
    EXPOSURE = "exposure"
    CONVERSION = "conversion"
    BEHAVIOUR = "behaviour"
    NONE = "none"


KNOWN: dict[str, tuple[Role, str]] = {
    "ActiveScriptWrappable": (Role.BEHAVIOUR, "keeps a wrapper alive while its object has pending activity"),
    "Affects": (Role.NONE, "tells V8's inspector whether a call has side effects"),
    "AllowShared": (Role.CONVERSION, "accepts a view on a SharedArrayBuffer"),
    "BufferSourceTypeNoSizeLimit": (Role.CONVERSION, "accepts a buffer larger than Blink's usual limit"),
    "CEReactions": (Role.BEHAVIOUR, "runs custom element reactions around the call"),
    "CachedAccessor": (Role.BEHAVIOUR, "caches what a getter returns in a private property of the wrapper"),
    "CachedAttribute": (Role.BEHAVIOUR, "returns the same value until the object says it changed"),
    "CallWith": (Role.NONE, "what Blink passes its class first; mitto's classes take the IDL arguments only"),
    "CheckSecurity": (Role.BEHAVIOUR, "checks that the caller may reach the returned object"),
    "Clamp": (Role.CONVERSION, "clamps a number into an integer type's range"),
    "ContextEnabled": (Role.EXPOSURE, "exposes a construct where the context turns a feature on"),
    "ConvertibleToObject": (Role.CONVERSION, "lets Blink convert a dictionary back to an object"),
    "CrossOrigin": (Role.PLAN, "lets a frame of another origin reach a member"),
    "CrossOriginIsolatedOrRuntimeEnabled": (Role.EXPOSURE, "exposes a member in isolated contexts or with a feature"),
    "DeprecateAs": (Role.NONE, "writes a deprecation warning to the console, which a page cannot read"),
    "EnforceRange": (Role.CONVERSION, "throws for a number outside an integer type's range"),
    "Exposed": (Role.EXPOSURE, "names the global objects that a construct is exposed on"),
    "GetterCallWith": (Role.NONE, "what Blink passes the getter of its class first"),
    "Global": (Role.PLAN, "makes an interface a global object, whose members go on the instance"),
    "HTMLConstructor": (Role.BEHAVIOUR, "constructs an element through the custom element registry"),
    "HasAsyncIteratorReturnAlgorithm": (Role.PLAN, "gives an async iterator a return() method"),
    "ImplementedAs": (Role.NONE, "the name of Blink's C++ class or method"),
    "IsCodeLike": (Role.BEHAVIOUR, "lets V8 treat an object as code for eval()"),
    "IsolatedContext": (Role.EXPOSURE, "exposes a construct in isolated web apps only"),
    "LegacyFactoryFunction": (Role.PLAN, "adds a factory function, such as Image, to the global object"),
    "LegacyFactoryFunction_CallWith": (Role.NONE, "what Blink passes its factory function first"),
    "LegacyFactoryFunction_RaisesException": (Role.BEHAVIOUR, "lets a factory function throw"),
    "LegacyLenientSetter": (Role.PLAN, "gives a read-only attribute a setter that does nothing"),
    "LegacyLenientThis": (Role.BEHAVIOUR, "returns undefined for the wrong receiver instead of throwing"),
    "LegacyNoInterfaceObject": (Role.PLAN, "leaves the interface object out of the global object"),
    "LegacyNullToEmptyString": (Role.CONVERSION, "converts null to the empty string"),
    "LegacyOverrideBuiltIns": (Role.BEHAVIOUR, "lets named properties shadow the prototype's"),
    "LegacyTreatNonObjectAsNull": (Role.CONVERSION, "converts a non-object callback to null"),
    "LegacyUnenumerableNamedProperties": (Role.BEHAVIOUR, "leaves named properties out of enumeration"),
    "LegacyUnforgeable": (Role.PLAN, "puts a member on each instance, not configurable"),
    "LegacyWindowAlias": (Role.PLAN, "adds another name for the interface object to the global object"),
    "LegacyWindowAlias_Measure": (Role.NONE, "counts uses of the alias"),
    "LegacyWindowAlias_MeasureAs": (Role.NONE, "counts uses of the alias"),
    "LegacyWindowAlias_RuntimeEnabled": (Role.EXPOSURE, "exposes the alias with a feature"),
    "LogActivity": (Role.NONE, "logs calls for extensions' activity log"),
    "LogAllWorlds": (Role.NONE, "logs calls for extensions' activity log"),
    "Measure": (Role.NONE, "counts uses for Chrome's statistics"),
    "MeasureAs": (Role.NONE, "counts uses for Chrome's statistics"),
    "NewObject": (Role.BEHAVIOUR, "returns a new object on every call"),
    "NoAllocDirectCall": (Role.PLAN, "installs a member after the others, with V8's fast calls"),
    "NodeWrapInOwnContext": (Role.BEHAVIOUR, "wraps a node in its own context"),
    "NotEnumerable": (Role.PLAN, "makes a member not enumerable"),
    "NotSubclassable": (Role.BEHAVIOUR, "refuses a constructor called from a subclass"),
    "PassAsSpan": (Role.CONVERSION, "passes a buffer to the class as a span"),
    "PerWorldBindings": (Role.NONE, "lets Blink write a faster binding for the main world"),
    "PermissiveDictionaryConversion": (Role.CONVERSION, "converts a non-object to an empty dictionary"),
    "PromiseIDLTypeMismatch": (Role.NONE, "lets Blink's class return another type of promise"),
    "PutForwards": (Role.PLAN, "gives a read-only attribute a setter that sets another attribute"),
    "RaisesException": (Role.BEHAVIOUR, "lets the class throw"),
    "Reflect": (Role.BEHAVIOUR, "reflects a content attribute"),
    "ReflectEmpty": (Role.BEHAVIOUR, "the value of a reflected attribute when it is empty"),
    "ReflectInvalid": (Role.BEHAVIOUR, "the value of a reflected attribute when it is invalid"),
    "ReflectMissing": (Role.BEHAVIOUR, "the value of a reflected attribute when it is missing"),
    "ReflectOnly": (Role.BEHAVIOUR, "the values a reflected attribute can have"),
    "Replaceable": (Role.PLAN, "gives a read-only attribute a setter that shadows it with a data property"),
    "RuntimeCallStatsCounter": (Role.NONE, "counts time spent for Chrome's statistics"),
    "RuntimeEnabled": (Role.EXPOSURE, "exposes a construct only when a runtime-enabled feature is on"),
    "SameObject": (Role.BEHAVIOUR, "returns the same object on every call"),
    "SaveSameObject": (Role.BEHAVIOUR, "keeps the object that SameObject returns"),
    "SecureContext": (Role.EXPOSURE, "exposes a construct in secure contexts only"),
    "Serializable": (Role.BEHAVIOUR, "lets structured cloning copy an object"),
    "SetterCallWith": (Role.NONE, "what Blink passes the setter of its class first"),
    "SupportsTaskAttribution": (Role.NONE, "lets Blink attribute tasks to a callback"),
    "Transferable": (Role.BEHAVIOUR, "lets structured cloning transfer an object"),
    "URL": (Role.BEHAVIOUR, "resolves a reflected attribute as a URL"),
    "Unscopable": (Role.PLAN, "adds a member to the prototype's @@unscopables"),
    "V8EnableIndexOf": (Role.BEHAVIOUR, "lets V8 find a value in an indexed collection"),
    "V8EnableIterableToList": (Role.BEHAVIOUR, "lets V8 turn an indexed collection into a list"),
}


def check(where: str, keys: list[str]) -> None:
    """Raises UnhandledError for an extended attribute that KNOWN does not list.

    Args:
        where: the construct the attributes are on, such as "attribute Event.type".
        keys: the names of its extended attributes.
    """
    for key in keys:
        if key not in KNOWN:
            raise UnhandledError(f"{where}: the generator does not handle the extended attribute [{key}]")
