#include "mitto/dom/html_script_element.h"

#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_script_element.h"
#include "mitto/dom/child_list_mutation_scope.h"
#include "mitto/dom/document.h"
#include "mitto/dom/event.h"
#include "mitto/dom/script_loader.h"
#include "mitto/dom/text.h"
#include "url/gurl.h"

namespace mitto {
namespace {

// Fires a trusted event of `type` at `target`, which does not bubble, as
// ScriptElementBase::DispatchLoadEvent() and DispatchErrorEvent() do.
void FireSimpleEvent(EventTarget& target, const std::u16string& type) {
  bindings::EventInit init;
  Event* const event = Event::Create(type, init);
  event->SetTrusted(true);
  target.DispatchEvent(event);
}

}  // namespace

// static
HTMLScriptElement* HTMLScriptElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLScriptElement>(document);
}

HTMLScriptElement::HTMLScriptElement(Document& document)
    : HTMLElement(document, u"script"),
      loader_(bindings::MakeGarbageCollected<ScriptLoader>(*this)) {}

const bindings::WrapperTypeInfo* HTMLScriptElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLScriptElement::kWrapperTypeInfo;
}

void HTMLScriptElement::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(loader_);
  HTMLElement::Trace(visitor);
}

std::u16string HTMLScriptElement::Src() const {
  // Element::GetURLAttribute(): the attribute, stripped of HTML spaces,
  // resolved against the document's base URL, or the attribute itself where
  // that is no URL.
  const std::optional<std::u16string> value = GetAttribute(u"src");
  if (!value) {
    return std::u16string();
  }
  const std::u16string stripped(
      base::TrimString(*value, u" \t\n\f\r", base::TRIM_ALL));
  const GURL url = GetDocument().CompleteUrl(stripped);
  return url.is_valid() ? base::UTF8ToUTF16(url.spec()) : *value;
}

void HTMLScriptElement::SetSrc(const std::u16string& src,
                               bindings::ExceptionState& exception_state) {
  SetAttributeWithoutValidation(u"src", src);
}

bool HTMLScriptElement::Async() const {
  return HasAttribute(u"async") || loader_->IsForceAsync();
}

void HTMLScriptElement::SetAsync(bool async) {
  SetReflectedBooleanAttribute(u"async", async);
  loader_->HandleAsyncAttribute();
}

void HTMLScriptElement::SetText(const std::u16string& text,
                                bindings::ExceptionState& exception_state) {
  // The DOM standard's "string replace all", one record for observers.
  const ChildListMutationScope mutation(*this);
  RemoveChildren();
  if (!text.empty()) {
    ParserAppendChild(mitto::Text::Create(GetDocument(), text));
  }
}

std::optional<std::u16string> HTMLScriptElement::ScriptTextContentForBinding()
    const {
  return TextContentForBinding();
}

void HTMLScriptElement::SetScriptTextContentForBinding(
    const std::optional<std::u16string>& value,
    bindings::ExceptionState& exception_state) {
  SetTextContentForBinding(value, exception_state);
}

// static
bool HTMLScriptElement::Supports(const std::u16string& type) {
  return type == u"classic" || type == u"module" || type == u"importmap" ||
         type == u"speculationrules" || type == u"webbundle";
}

void HTMLScriptElement::SetCreatedByParser(Document& parser_document) {
  loader_->SetCreatedByParser(parser_document);
}

void HTMLScriptElement::SetAlreadyStarted() {
  loader_->SetAlreadyStarted();
}

void HTMLScriptElement::SetStartPosition(int line, int column) {
  start_line_ = line;
  start_column_ = column;
}

bool HTMLScriptElement::IsPotentiallyRenderBlocking() const {
  if (const std::optional<std::u16string> blocking =
          GetAttribute(u"blocking")) {
    for (const std::u16string_view token :
         base::SplitStringPiece(*blocking, u" \t\n\f\r", base::KEEP_WHITESPACE,
                                base::SPLIT_WANT_NONEMPTY)) {
      if (token == u"render") {
        return true;
      }
    }
  }
  return loader_->IsParserInserted() &&
         loader_->GetScriptType() ==
             ScriptLoader::ScriptTypeAtPrepare::kClassic &&
         !HasAttribute(u"async") && !HasAttribute(u"defer");
}

void HTMLScriptElement::DispatchLoadEvent() {
  FireSimpleEvent(*this, u"load");
}

void HTMLScriptElement::DispatchErrorEvent() {
  FireSimpleEvent(*this, u"error");
}

Node::InsertionNotificationRequest HTMLScriptElement::InsertedInto(
    Node& insertion_point) {
  HTMLElement::InsertedInto(insertion_point);
  // The script prepares itself once every node of the insertion is in.
  return insertion_point.IsConnected()
             ? InsertionNotificationRequest::
                   kInsertionShouldCallDidNotifySubtreeInsertions
             : InsertionNotificationRequest::kInsertionDone;
}

void HTMLScriptElement::DidNotifySubtreeInsertionsToDocument() {
  loader_->DidNotifySubtreeInsertionsToDocument();
}

void HTMLScriptElement::ChildrenChanged(ChildrenChangeType type,
                                        Node& child,
                                        bool by_parser) {
  HTMLElement::ChildrenChanged(type, child, by_parser);
  loader_->ChildrenChanged(type == ChildrenChangeType::kElementInserted ||
                           type == ChildrenChangeType::kNonElementInserted);
}

void HTMLScriptElement::AttributeChanged(const QualifiedName& name,
                                         const std::u16string* old_value,
                                         const std::u16string* new_value) {
  HTMLElement::AttributeChanged(name, old_value, new_value);
  // HTMLScriptElement::ParseAttribute(), for attributes of no namespace.
  if (name.namespace_uri) {
    return;
  }
  if (name.local_name == u"src") {
    loader_->HandleSourceAttribute(new_value ? *new_value : std::u16string());
  } else if (name.local_name == u"async") {
    loader_->HandleAsyncAttribute();
  }
}

Node* HTMLScriptElement::CloneWithoutChildren(Document& document) const {
  // The HTML Standard's cloning steps: a copy of a script that has already
  // started has too.
  auto* const copy = static_cast<HTMLScriptElement*>(
      HTMLElement::CloneWithoutChildren(document));
  if (loader_->AlreadyStarted()) {
    copy->Loader().SetAlreadyStarted();
  }
  return copy;
}

}  // namespace mitto
