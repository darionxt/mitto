#include "mitto/dom/html_base_element.h"

#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_base_element.h"
#include "mitto/dom/document.h"
#include "url/gurl.h"

namespace mitto {

// static
HTMLBaseElement* HTMLBaseElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLBaseElement>(document);
}

HTMLBaseElement::HTMLBaseElement(Document& document)
    : HTMLElement(document, u"base") {}

const bindings::WrapperTypeInfo* HTMLBaseElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLBaseElement::kWrapperTypeInfo;
}

std::u16string HTMLBaseElement::Href() const {
  const std::u16string* const value = FindAttribute(u"href");
  if (!value) {
    return GetDocument().UrlForBinding();
  }
  const std::u16string_view stripped =
      base::TrimString(*value, u" \t\n\f\r", base::TRIM_ALL);
  const GURL url = GetDocument().FallbackBaseUrl().Resolve(stripped);
  if (!url.is_valid()) {
    return std::u16string(stripped);
  }
  return base::UTF8ToUTF16(url.spec());
}

void HTMLBaseElement::SetHref(const std::u16string& href) {
  SetAttributeWithoutValidation(u"href", href);
}

Node::InsertionNotificationRequest HTMLBaseElement::InsertedInto(
    Node& insertion_point) {
  HTMLElement::InsertedInto(insertion_point);
  if (insertion_point.IsConnected()) {
    GetDocument().ProcessBaseElement();
  }
  return InsertionNotificationRequest::kInsertionDone;
}

void HTMLBaseElement::RemovedFrom(Node& insertion_point) {
  HTMLElement::RemovedFrom(insertion_point);
  if (insertion_point.IsConnected()) {
    GetDocument().ProcessBaseElement();
  }
}

void HTMLBaseElement::AttributeChanged(const QualifiedName& name,
                                       const std::u16string* old_value,
                                       const std::u16string* new_value) {
  HTMLElement::AttributeChanged(name, old_value, new_value);
  // HTMLBaseElement::ParseAttribute(): href and target, of no namespace.
  if (!name.namespace_uri &&
      (name.local_name == u"href" || name.local_name == u"target") &&
      IsConnected()) {
    GetDocument().ProcessBaseElement();
  }
}

}  // namespace mitto
