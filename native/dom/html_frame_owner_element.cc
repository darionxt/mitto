#include "mitto/dom/html_frame_owner_element.h"

#include <utility>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/dom/document.h"
#include "mitto/dom/event.h"
#include "mitto/dom/frame.h"
#include "mitto/dom/html_parser_idioms.h"
#include "mitto/dom/permissions_policy.h"
#include "mitto/dom/permissions_policy_parser.h"
#include "mitto/dom/window.h"
#include "url/gurl.h"
#include "url/url_constants.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

namespace {

// The URL of a srcdoc document, which a frame with srcdoc shows.
constexpr char16_t kSrcdocUrl[] = u"about:srcdoc";

}  // namespace

HTMLFrameOwnerElement::HTMLFrameOwnerElement(mitto::Document& document,
                                             const std::u16string& local_name)
    : HTMLElement(document, local_name) {}

HTMLFrameOwnerElement::~HTMLFrameOwnerElement() = default;

void HTMLFrameOwnerElement::Trace(cppgc::Visitor* visitor) const {
  HTMLElement::Trace(visitor);
  visitor->Trace(feature_policy_);
}

Window* HTMLFrameOwnerElement::ContentWindow() const {
  return content_frame_ ? content_frame_->DomWindow() : nullptr;
}

mitto::Document* HTMLFrameOwnerElement::ContentDocument() const {
  Window* const window = ContentWindow();
  if (!window || !window->HasDocument()) {
    return nullptr;
  }
  // The HTML Standard's contentDocument: null unless the document's origin is
  // same origin-domain with the current settings object's. Without
  // document.domain (#286), that is the same origin.
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (isolate && isolate->InContext() &&
      !Window::Current().GetSecurityOrigin().IsSameOriginWith(
          window->GetDocument().GetOrigin())) {
    return nullptr;
  }
  return &window->GetDocument();
}

void HTMLFrameOwnerElement::DispatchLoad() {
  bindings::EventInit init;
  Event* const event = Event::Create(u"load", init);
  event->SetTrusted(true);
  DispatchEvent(event);
}

Node::InsertionNotificationRequest HTMLFrameOwnerElement::InsertedInto(
    Node& insertion_point) {
  HTMLElement::InsertedInto(insertion_point);
  return InsertionNotificationRequest::
      kInsertionShouldCallDidNotifySubtreeInsertions;
}

void HTMLFrameOwnerElement::DidNotifySubtreeInsertionsToDocument() {
  // HTMLFrameElementBase::DidNotifySubtreeInsertionsToDocument(): only in a
  // document that has a frame, and only once.
  if (!IsConnected() || content_frame_) {
    return;
  }
  Window* const window = GetDocument().DefaultView();
  if (!window || !window->GetFrame()) {
    return;
  }
  // SetNameAndOpenURL().
  const std::u16string* const name = FindAttribute(u"name");
  frame_name_ = name ? *name : std::u16string();
  OpenURL();
}

void HTMLFrameOwnerElement::RemovedFrom(Node& insertion_point) {
  HTMLElement::RemovedFrom(insertion_point);
  // ChildFrameDisconnector: a frame whose element leaves the document goes,
  // but for one that moveBefore() moves, which keeps its frame.
  if (!IsConnected() && !GetDocument().StatePreservingAtomicMoveInProgress()) {
    DisconnectContentFrame();
  }
}

void HTMLFrameOwnerElement::AttributeChanged(const QualifiedName& name,
                                             const std::u16string* old_value,
                                             const std::u16string* new_value) {
  HTMLElement::AttributeChanged(name, old_value, new_value);
  ParseFrameAttribute(name, old_value, new_value);
}

void HTMLFrameOwnerElement::DidCloneAttributes() {
  HTMLElement::DidCloneAttributes();
  for (const Attribute& attribute : attributes()) {
    ParseFrameAttribute(attribute.name, nullptr, &attribute.value);
  }
}

void HTMLFrameOwnerElement::ParseFrameAttribute(
    const QualifiedName& name,
    const std::u16string* old_value,
    const std::u16string* new_value) {
  if (name.namespace_uri) {
    return;
  }
  if (name.local_name == u"srcdoc") {
    if (new_value) {
      SetLocation(kSrcdocUrl);
    } else if (const std::u16string* const src = FindAttribute(u"src")) {
      SetLocation(std::u16string(StripHTMLSpaces(*src)));
    } else if (old_value) {
      // A srcdoc that goes with no src left loads about:blank
      // (https://crbug.com/1233143).
      SetLocation(base::ASCIIToUTF16(std::string_view(url::kAboutBlankURL)));
    }
  } else if (name.local_name == u"src" && !FindAttribute(u"srcdoc")) {
    SetLocation(new_value ? std::u16string(StripHTMLSpaces(*new_value))
                          : std::u16string());
  } else if (name.local_name == u"id" || name.local_name == u"name") {
    frame_name_ = new_value ? *new_value : std::u16string();
  } else if (name.local_name == u"sandbox" && HasHTMLTagName(u"iframe")) {
    // It holds from the frame's next navigation on.
    sandbox_flags_ = SandboxFlags::kNone;
    if (new_value) {
      const SandboxPolicy policy =
          ParseSandboxPolicy(base::UTF16ToUTF8(*new_value));
      sandbox_flags_ = policy.flags;
      Window* const window = GetDocument().DefaultView();
      if (!policy.error_message.empty() && window) {
        window->AddConsoleMessage(
            ConsoleLevel::kError,
            base::StrCat({"Error while parsing the 'sandbox' attribute: ",
                          policy.error_message}));
      }
    }
    UpdateContainerPolicy();
  } else if ((name.local_name == u"allow" ||
              name.local_name == u"allowfullscreen" ||
              name.local_name == u"allowpaymentrequest") &&
             HasHTMLTagName(u"iframe")) {
    UpdateContainerPolicy();
  }
}

PermissionsPolicy* HTMLFrameOwnerElement::FeaturePolicy() {
  if (!feature_policy_) {
    // A document of no window is in its creator's execution context.
    Window* const window = GetDocument().DefaultView()
                               ? GetDocument().DefaultView()
                               : &Window::Current();
    feature_policy_ = PermissionsPolicy::Create(
        window->GetDocument(), FrameState(window->GetDocument()));
  }
  return feature_policy_.Get();
}

std::unique_ptr<PermissionsPolicyState> HTMLFrameOwnerElement::FrameState(
    Document& parent) const {
  return parent.NewPermissionsPolicyState(&parent.GetPermissionsPolicyState(),
                                          container_policy_,
                                          OriginForPermissionsPolicy());
}

url::Origin HTMLFrameOwnerElement::OriginForPermissionsPolicy() const {
  // MakeOriginForPermissionsPolicy(): a sandboxed document's origin is opaque,
  // and one that inherits its origin has its parent's.
  if ((sandbox_flags_ & SandboxFlags::kOrigin) != SandboxFlags::kNone) {
    return url::Origin();
  }
  const GURL url = GetDocument().CompleteUrl(url_);
  return url_.empty() || url.IsAboutBlank() || url.IsAboutSrcdoc()
             ? GetDocument().GetOrigin()
             : url::Origin::Create(url);
}

void HTMLFrameOwnerElement::UpdateContainerPolicy() {
  Window* const window = GetDocument().DefaultView();
  container_policy_.clear();
  if (!HasHTMLTagName(u"iframe")) {
    // GetLegacyFramePolicies(): a frame element's documents have no fullscreen.
    container_policy_.push_back(
        {.feature = base::raw_ref(PolicyFeatureNamed("fullscreen"))});
    return;
  }
  if (!window) {
    return;
  }
  // HTMLIFrameElement::ConstructContainerPolicy().
  std::vector<PolicyMessage> messages;
  const std::u16string* const allow = FindAttribute(u"allow");
  container_policy_ = ParseAllowAttribute(
      allow ? base::UTF16ToUTF8(*allow) : std::string(),
      window->GetSecurityOrigin(), OriginForPermissionsPolicy(),
      bindings::PerIsolateData::From(v8::Isolate::GetCurrent())->features(),
      messages);
  for (const auto& [attribute, feature] :
       {std::pair(u"allowfullscreen", "fullscreen"),
        std::pair(u"allowpaymentrequest", "payment")}) {
    if (FindAttribute(attribute) &&
        !AllowEverywhereIfNotPresent(feature, container_policy_)) {
      messages.push_back(
          {.text = base::StrCat({"Allow attribute will take precedence over '",
                                 base::UTF16ToUTF8(attribute), "'."})});
    }
  }
  if (feature_policy_) {
    feature_policy_->SetFrameState(FrameState(window->GetDocument()));
  }
  for (PolicyMessage& message : messages) {
    window->AddConsoleMessageOnce(message.level, std::move(message.text));
  }
}

void HTMLFrameOwnerElement::SetLocation(std::u16string location) {
  url_ = std::move(location);
  if (IsConnected()) {
    OpenURL();
  }
}

void HTMLFrameOwnerElement::OpenURL() {
  Window* const window = GetDocument().DefaultView();
  Frame* const parent = window ? window->GetFrame() : nullptr;
  if (!parent) {
    return;
  }
  // An empty URL is about:blank's.
  const GURL url = url_.empty() ? GURL(url::kAboutBlankURL)
                                : GetDocument().CompleteUrl(url_);
  UpdateContainerPolicy();
  if (!content_frame_) {
    content_frame_ = parent->CreateChildFrame(*this, frame_name_);
    if (!content_frame_) {
      return;
    }
  }
  content_frame_->NavigateFromOwner(url);
}

void HTMLFrameOwnerElement::DisconnectContentFrame() {
  Frame* const frame = content_frame_;
  if (!frame) {
    return;
  }
  // The frame clears itself from the element as it goes.
  frame->Detach();
  content_frame_ = nullptr;
}

}  // namespace mitto
