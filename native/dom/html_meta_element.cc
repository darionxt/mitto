#include "mitto/dom/html_meta_element.h"

#include "base/strings/string_util.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_meta_element.h"
#include "mitto/dom/document.h"
#include "mitto/dom/http_refresh_scheduler.h"
#include "mitto/dom/qualified_name.h"

namespace mitto {

// static
HTMLMetaElement* HTMLMetaElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLMetaElement>(document);
}

HTMLMetaElement::HTMLMetaElement(Document& document)
    : HTMLElement(document, u"meta") {}

const bindings::WrapperTypeInfo* HTMLMetaElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLMetaElement::kWrapperTypeInfo;
}

Node::InsertionNotificationRequest HTMLMetaElement::InsertedInto(
    Node& insertion_point) {
  HTMLElement::InsertedInto(insertion_point);
  return InsertionNotificationRequest::
      kInsertionShouldCallDidNotifySubtreeInsertions;
}

void HTMLMetaElement::DidNotifySubtreeInsertionsToDocument() {
  ProcessHttpEquiv();
}

void HTMLMetaElement::AttributeChanged(const QualifiedName& name,
                                       const std::u16string* old_value,
                                       const std::u16string* new_value) {
  HTMLElement::AttributeChanged(name, old_value, new_value);
  if (!name.namespace_uri &&
      (name.local_name == u"content" || name.local_name == u"http-equiv")) {
    ProcessHttpEquiv();
  }
}

void HTMLMetaElement::ProcessHttpEquiv() {
  // In the document's tree, which no shadow tree is yet.
  if (!IsConnected()) {
    return;
  }
  const std::u16string* const content = FindAttribute(u"content");
  const std::u16string* const http_equiv = FindAttribute(u"http-equiv");
  if (!content || !http_equiv || http_equiv->empty()) {
    return;
  }
  if (base::EqualsCaseInsensitiveASCII(*http_equiv, u"refresh")) {
    GetDocument().GetHttpRefreshScheduler().MaybeHandle(
        *content, HttpRefreshScheduler::Source::kMetaTag);
  }
}

}  // namespace mitto
