// An element that holds a frame, `iframe` or `frame`, as Blink's
// HTMLFrameOwnerElement and HTMLFrameElementBase have it: once it is in a
// document that has a frame, it makes a child frame of that frame, named by
// its name, and navigates it to its src, or to about:blank; a change of its src
// navigates the frame again; and once it leaves the document, the frame goes.
// Its load event fires once its frame's window's has.
//
// Upstream: third_party/blink/renderer/core/html/html_frame_owner_element.cc
// Upstream: third_party/blink/renderer/core/html/html_frame_element_base.cc

#ifndef MITTO_DOM_HTML_FRAME_OWNER_ELEMENT_H_
#define MITTO_DOM_HTML_FRAME_OWNER_ELEMENT_H_

#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "mitto/dom/html_element.h"
#include "mitto/dom/permissions_policy_state.h"
#include "mitto/dom/sandbox_flags.h"
#include "url/origin.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class Document;
class Frame;
class PermissionsPolicy;
class Window;

// Blink's mojom::DeferredFetchPolicy: the quota of fetchLater() that a frame's
// element reserves for its documents, none, the normal 64 KiB or the minimal
// 8 KiB.
enum class DeferredFetchPolicy {
  kDisabled,
  kDeferredFetch,
  kDeferredFetchMinimal
};

class HTMLFrameOwnerElement : public HTMLElement {
 public:
  ~HTMLFrameOwnerElement() override;

  // contentWindow: the window of the element's frame, or null without one.
  Window* ContentWindow() const;
  // contentDocument: the document of the element's frame, if its origin is
  // the current window's, as the HTML Standard's getter and Blink's
  // [CheckSecurity=ReturnValue] check it, and otherwise null.
  mitto::Document* ContentDocument() const;

  // What the Page uses.
  //
  // The element's frame, or null.
  Frame* ContentFrame() const { return content_frame_; }
  // What its sandbox attribute withholds from the documents that its frame
  // navigates to, which is an iframe's alone.
  SandboxFlags GetSandboxFlags() const { return sandbox_flags_; }
  // What it lets a document that its frame navigates to inherit of its own
  // document's permissions policy, which an iframe's allow says.
  const ParsedPolicy& GetContainerPolicy() const { return container_policy_; }
  // iframe.featurePolicy.
  PermissionsPolicy* FeaturePolicy();
  // The frame goes, as its documents have unloaded: the element holds none.
  void ClearContentFrame() { content_frame_ = nullptr; }
  // The quota of fetchLater() that the element reserves, as its FramePolicy's
  // deferred_fetch_policy holds it, which a navigation that its frame's
  // parent starts sets and a document of the parent's origin clears.
  DeferredFetchPolicy GetDeferredFetchPolicy() const {
    return deferred_fetch_policy_;
  }
  void SetDeferredFetchPolicy(DeferredFetchPolicy policy) {
    deferred_fetch_policy_ = policy;
  }
  // HTMLFrameOwnerElement::DispatchLoad(): load at the element, once its
  // frame's window's load event has been dispatched.
  void DispatchLoad();

  void Trace(cppgc::Visitor* visitor) const override;

  // Node:
  InsertionNotificationRequest InsertedInto(Node& insertion_point) override;
  void DidNotifySubtreeInsertionsToDocument() override;
  void RemovedFrom(Node& insertion_point) override;

 protected:
  HTMLFrameOwnerElement(mitto::Document& document,
                        const std::u16string& local_name);

  // Element:
  void AttributeChanged(const QualifiedName& name,
                        const std::u16string* old_value,
                        const std::u16string* new_value) override;
  void DidCloneAttributes() override;

 private:
  // HTMLFrameElementBase::ParseAttribute() and HTMLIFrameElement's: src,
  // srcdoc, name, id and sandbox.
  void ParseFrameAttribute(const QualifiedName& name,
                           const std::u16string* old_value,
                           const std::u16string* new_value);
  // Works out the container policy again, as Blink's UpdateContainerPolicy()
  // does when an attribute that it comes from changes and before each load.
  void UpdateContainerPolicy();
  // The origin of the document that its frame is to show.
  url::Origin OriginForPermissionsPolicy() const;
  // What that document's permissions policy would allow.
  std::unique_ptr<PermissionsPolicyState> FrameState(Window& window) const;
  // HTMLFrameElementBase::SetLocation(): the URL the frame is to show, which
  // it navigates to now if the element is connected.
  void SetLocation(std::u16string location);
  // HTMLFrameElementBase::OpenURL() and
  // HTMLFrameOwnerElement::LoadOrRedirectSubframe(): makes the element's frame
  // if it has none, and navigates it to its URL, about:blank for none.
  void OpenURL();
  // HTMLFrameOwnerElement::DisconnectContentFrame(): the element's frame
  // goes, with its documents.
  void DisconnectContentFrame();

  raw_ptr<Frame> content_frame_ = nullptr;
  // The URL the frame is to show, as src or srcdoc gives it, before it is
  // resolved against the document's base URL.
  std::u16string url_;
  // The name the frame gets when the element makes it: its name, or its ID.
  std::u16string frame_name_;
  SandboxFlags sandbox_flags_ = SandboxFlags::kNone;
  ParsedPolicy container_policy_;
  // An iframe's featurePolicy.
  cppgc::Member<PermissionsPolicy> feature_policy_;
  DeferredFetchPolicy deferred_fetch_policy_ = DeferredFetchPolicy::kDisabled;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_FRAME_OWNER_ELEMENT_H_
