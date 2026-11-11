"""How the generator names C++ files, classes, functions and members after Web IDL names."""

# Upstream: third_party/blink/renderer/bindings/scripts/bind_gen/name_style.py

import re

__all__ = ["file_name", "function_name", "member_name"]


def _words(name: str) -> list[str]:
    """Splits a Web IDL name into words: "DOMException" into DOM and Exception, "hasUAVisualTransition" into has, UA,
    Visual and Transition."""
    return re.findall(r"[A-Z]+(?=[A-Z][a-z0-9])|[A-Z]?[a-z0-9]+|[A-Z]+", name)


def file_name(name: str) -> str:
    """The base name of a file for a definition, such as "pop_state_event" for PopStateEvent."""
    return "_".join(word.lower() for word in _words(name))


def function_name(name: str) -> str:
    """The C++ name of a method for an operation or attribute, such as "InitEvent" or "HasUAVisualTransition"."""
    return "".join(word[0].upper() + word[1:] for word in _words(name))


def member_name(name: str) -> str:
    """The C++ name of a dictionary member, such as "has_ua_visual_transition"."""
    return "_".join(word.lower() for word in _words(name))
