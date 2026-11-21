#include "mitto/dom/html_meta_element.h"

#include "base/strings/string_util.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_meta_element.h"
#include "mitto/dom/document.h"
#include "mitto/dom/http_refresh_scheduler.h"
#include "mitto/dom/qualified_name.h"
#include "mitto/dom/window.h"

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
  ProcessContent();
  ProcessHttpEquiv();
}

void HTMLMetaElement::AttributeChanged(const QualifiedName& name,
                                       const std::u16string* old_value,
                                       const std::u16string* new_value) {
  HTMLElement::AttributeChanged(name, old_value, new_value);
  if (name.namespace_uri) {
    return;
  }
  if (name.local_name == u"name" || name.local_name == u"content" ||
      name.local_name == u"media") {
    ProcessContent();
  }
  if (name.local_name == u"content" || name.local_name == u"http-equiv") {
    ProcessHttpEquiv();
  }
}

void HTMLMetaElement::ProcessContent() {
  // In the document's tree, which no shadow tree is yet, of a document that
  // a window shows.
  if (!IsConnected()) {
    return;
  }
  const std::u16string* const name = FindAttribute(u"name");
  const std::u16string* const content = FindAttribute(u"content");
  if (!name || name->empty() || !content) {
    return;
  }
  Document& document = GetDocument();
  Window* const window = document.DefaultView();
  if (base::EqualsCaseInsensitiveASCII(*name, u"referrer") && window &&
      window->HasContext() && &window->GetDocument() == &document) {
    document.ParseAndSetReferrerPolicy(
        *content, Document::ReferrerPolicySource::kMetaTag);
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
  // HttpEquiv::Process().
  if (base::EqualsCaseInsensitiveASCII(*http_equiv, u"refresh")) {
    GetDocument().GetHttpRefreshScheduler().MaybeHandle(
        *content, HttpRefreshScheduler::Source::kMetaTag);
  } else if (base::EqualsCaseInsensitiveASCII(*http_equiv,
                                              u"content-language")) {
    GetDocument().SetContentLanguage(*content);
  }
}

}  // namespace mitto
