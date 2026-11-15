#include "mitto/dom/html_anchor_element.h"

#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_anchor_element.h"
#include "mitto/dom/child_list_mutation_scope.h"
#include "mitto/dom/document.h"
#include "mitto/dom/event.h"
#include "mitto/dom/frame_loader.h"
#include "mitto/dom/html_parser_idioms.h"
#include "mitto/dom/resolved_url.h"
#include "mitto/dom/text.h"
#include "mitto/dom/window.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace mitto {

HTMLAnchorElementBase::HTMLAnchorElementBase(Document& document,
                                             const std::u16string& local_name)
    : HTMLElement(document, local_name) {}

ResolvedURL HTMLAnchorElementBase::Url() const {
  const std::u16string* href = FindAttribute(u"href");
  if (!href) {
    return ResolvedURL();
  }
  // Blink's Href() resolves it with the document's CompleteURL(), which
  // encodes the query in the document's encoding. A URL that does not resolve
  // is none, as Blink's Url() has it.
  const GURL url = GetDocument().CompleteUrl(StripHTMLSpaces(*href));
  if (!url.is_valid()) {
    return ResolvedURL();
  }
  return ResolvedURL(base::UTF8ToUTF16(url.spec()));
}

void HTMLAnchorElementBase::SetUrl(const ResolvedURL& url) {
  SetAttributeWithoutValidation(u"href", url.GetString());
}

std::u16string HTMLAnchorElementBase::Input() const {
  const std::u16string* href = FindAttribute(u"href");
  return href ? *href : std::u16string();
}

void HTMLAnchorElementBase::SetHref(const std::u16string& value) {
  SetAttributeWithoutValidation(u"href", value);
}

bool HTMLAnchorElementBase::Draggable() const {
  const std::u16string* value = FindAttribute(u"draggable");
  if (value && base::EqualsCaseInsensitiveASCII(*value, u"true")) {
    return true;
  }
  if (value && base::EqualsCaseInsensitiveASCII(*value, u"false")) {
    return false;
  }
  return FindAttribute(u"href");
}

int32_t HTMLAnchorElementBase::DefaultTabIndex() const {
  return 0;
}

bool HTMLAnchorElementBase::HasActivationBehavior() const {
  return FindAttribute(u"href");
}

void HTMLAnchorElementBase::RunActivationBehaviorBeforeDOMActivate(
    Event& event,
    EventDispatchHandlingState* state) {
  if (event.DefaultPrevented() || event.DefaultHandled()) {
    return;
  }
  // AnchorElementUtils::IsLinkClick() and IsLiveLink(): a click of a mouse
  // event at a link. Its button is the left one, as click() gives: a Known
  // Gap for a page's own MouseEvent of another button.
  if (event.Type() == u"click" && event.IsMouseEvent() &&
      FindAttribute(u"href")) {
    HandleClick(event);
  }
}

namespace {

// Document::BaseTarget(): the target of the first base element that has one,
// in tree order.
const std::u16string* BaseTarget(Document& document) {
  for (Node* node = document.FirstChild(); node;
       node = node->NextInTree(&document)) {
    if (!node->IsElementNode()) {
      continue;
    }
    auto* const element = static_cast<Element*>(node);
    if (element->HasHTMLTagName(u"base")) {
      if (const std::u16string* target = element->FindAttribute(u"target")) {
        return target;
      }
    }
  }
  return nullptr;
}

}  // namespace

void HTMLAnchorElementBase::HandleClick(Event& event) {
  event.SetDefaultHandled();
  Document& document = GetDocument();
  Window* const window = document.DefaultView();
  if (!window) {
    return;
  }
  // An area element that is not connected follows nothing; an a element does.
  if (!IsConnected() && HasHTMLTagName(u"area")) {
    return;
  }
  const std::u16string* const href = FindAttribute(u"href");
  if (!href) {
    return;
  }
  const GURL url = document.CompleteUrl(StripHTMLSpaces(*href));
  // ping sends nothing, and referrerpolicy changes no Referer: Known Gaps,
  // since no Request Kind of a Persona holds them yet.
  //
  // download, for a URL whose content the document can read: Chrome hands it
  // to its download manager, with no navigation. A Known Gap, which starts no
  // request.
  if (FindAttribute(u"download") &&
      (url.SchemeIs("data") ||
       url::Origin::Create(GURL(document.UrlForBinding()))
           .IsSameOriginWith(url))) {
    return;
  }
  NavigateToHyperlink(url);
}

void HTMLAnchorElementBase::NavigateToHyperlink(const GURL& url) {
  Window* const window = GetDocument().DefaultView();
  if (!window) {
    return;
  }
  // GetEffectiveTarget(): the target attribute, else the document's base
  // target, which FrameTree::FindFrameForNavigationInternal() looks for:
  // _self, _top, _parent and none name the Page's one frame. Any other name,
  // _blank among them, opens a new window, which Chrome's pop-up blocker
  // refuses to a click that the user did not make, and then nothing
  // navigates.
  const std::u16string* target = FindAttribute(u"target");
  if (!target) {
    target = BaseTarget(GetDocument());
  }
  if (target && !target->empty() &&
      !base::EqualsCaseInsensitiveASCII(*target, u"_self") &&
      !base::EqualsCaseInsensitiveASCII(*target, u"_top") &&
      !base::EqualsCaseInsensitiveASCII(*target, u"_parent")) {
    return;
  }
  FrameLoadRequest request;
  request.url = url;
  request.reason = ClientNavigationReason::kAnchorClick;
  request.source_element = this;
  Navigate(*window, request, FrameLoadType::kStandard);
}

// static
HTMLAnchorElement* HTMLAnchorElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLAnchorElement>(document);
}

HTMLAnchorElement::HTMLAnchorElement(Document& document)
    : HTMLAnchorElementBase(document, u"a") {}

const bindings::WrapperTypeInfo* HTMLAnchorElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLAnchorElement::kWrapperTypeInfo;
}

void HTMLAnchorElement::SetTextContent(const std::u16string& text) {
  // The DOM standard's "string replace all", one record for observers.
  const ChildListMutationScope mutation(*this);
  RemoveChildren();
  if (!text.empty()) {
    ParserAppendChild(Text::Create(GetDocument(), text));
  }
}

}  // namespace mitto
