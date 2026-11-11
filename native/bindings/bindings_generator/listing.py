"""A listing of every interface and member that the generator plans, one per line, for review.

The release routine compares the listing of the old Engine Release with the new one's, so that the run's pull request
shows which interfaces and members a page gains or loses, and which features they are behind.
"""

from bindings_generator.install_plan import Phase, Property, plan_interface
from bindings_generator.model import Interface

__all__ = ["write_listing"]


def _describe(property_: Property) -> str:
    if property_.kind == "accessor":
        text = "get set" if property_.setter else "get"
    elif property_.kind == "method":
        text = f"method, length {property_.length}"
    elif property_.value is not None and not isinstance(property_.value, str):
        text = f"= {str(property_.value).lower() if isinstance(property_.value, bool) else property_.value}"
    else:
        text = "value"
    flags = [
        flag
        for flag, applies in (
            ("not enumerable", not property_.enumerable and property_.name != "constructor"),
            ("not configurable", not property_.configurable),
            ("per context", property_.phase is Phase.CONTEXT),
        )
        if applies
    ]
    condition = property_.condition.describe()
    return ", ".join([text, *flags]) + (f" {condition}" if condition else "")


def _owner(interface: Interface, target: str) -> str:
    if target == "interface":
        return interface.name
    if target == "prototype":
        return f"{interface.name}.prototype"
    return f"{interface.name} instance"


def write_listing(interfaces: list[Interface]) -> str:
    """Lists the interfaces and namespaces, by name, each with its own properties in the order Chrome installs them."""
    lines: list[str] = []
    for interface in sorted(interfaces, key=lambda item: item.name):
        if interface.kind not in ("interface", "namespace"):
            continue
        heading = f"{interface.kind} {interface.name}"
        if interface.parent:
            heading += f" : {interface.parent}"
        exposure = ", ".join(
            name if feature is None else f"{name} with {feature}" for name, feature in interface.exposure
        )
        if exposure:
            heading += f", exposed on {exposure}"
        if "LegacyNoInterfaceObject" in interface.extended_attributes:
            heading += ", no interface object"
        condition = interface.condition.describe()
        lines.append(heading + (f" {condition}" if condition else ""))
        if interface.kind == "interface":
            length = interface.constructor_group.length if interface.constructor_group else 0
            constructible = "" if interface.constructor_group else ", not constructible"
            lines.append(f"  {interface.name}.length = {length}{constructible}")
        for property_ in plan_interface(interface):
            lines.append(f"  {_owner(interface, property_.target)}.{property_.name}: {_describe(property_)}")
    return "\n".join(lines) + "\n"
