// The name of an element or an attribute: its namespace prefix, local name and
// namespace, and the DOM standard's "validate and extract" that a page's
// methods naming a namespace run on a qualified name.
//
// Upstream: third_party/blink/renderer/core/dom/qualified_name.h
// Upstream: third_party/blink/renderer/core/dom/qualified_name.cc
// Upstream: third_party/blink/renderer/core/dom/document.cc

#ifndef MITTO_DOM_QUALIFIED_NAME_H_
#define MITTO_DOM_QUALIFIED_NAME_H_

#include <optional>
#include <string>
#include <string_view>

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

struct QualifiedName {
  // What a qualified name's local name must be, as Blink's
  // Document::QualifiedNameParsingMode has it.
  enum class ParsingMode {
    kElement,
    kAttribute,
  };

  // Blink's Document::ParseQualifiedName(): the name that `qualified_name`
  // makes in `namespace_uri`, an empty one being null, or nothing after
  // throwing Chrome's InvalidCharacterError. Its namespace is not checked.
  static std::optional<QualifiedName> Parse(
      const std::optional<std::u16string>& namespace_uri,
      const std::u16string& qualified_name,
      ParsingMode mode,
      bindings::ExceptionState& exception_state);

  // Blink's Document::HasValidNamespaceForElements(), which attributes share:
  // a prefix needs a namespace, "xml" the XML namespace, and "xmlns" and only
  // it the XMLNS namespace.
  bool HasValidNamespace() const;

  // The prefix and local name, joined by ':' when there is a prefix.
  std::u16string ToString() const;

  // Whether `other` has the same local name and namespace, whatever their
  // prefixes, as Blink's QualifiedName::Matches().
  bool Matches(const QualifiedName& other) const {
    return local_name == other.local_name &&
           namespace_uri == other.namespace_uri;
  }

  friend bool operator==(const QualifiedName&, const QualifiedName&) = default;

  // Null for none. A prefix is never empty.
  std::optional<std::u16string> prefix;
  std::u16string local_name;
  // Null for none. A namespace is never empty.
  std::optional<std::u16string> namespace_uri;
};

// The DOM standard's valid element local name, which createElement() checks,
// as Blink's IsValidElementName() has it.
bool IsValidElementLocalName(std::u16string_view name);

// The DOM standard's valid attribute local name, which setAttribute() and
// toggleAttribute() check, as Blink's Document::IsValidAttributeLocalName()
// has it.
bool IsValidAttributeLocalName(std::u16string_view name);

}  // namespace mitto

#endif  // MITTO_DOM_QUALIFIED_NAME_H_
