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

#include <string>

#include "base/memory/raw_ptr.h"
#include "mitto/dom/html_element.h"
#include "mitto/dom/sandbox_flags.h"

namespace mitto {

class Document;
class Frame;
class Window;

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
  // The frame goes, as its documents have unloaded: the element holds none.
  void ClearContentFrame() { content_frame_ = nullptr; }
  // HTMLFrameOwnerElement::DispatchLoad(): load at the element, once its
  // frame's window's load event has been dispatched.
  void DispatchLoad();

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
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_FRAME_OWNER_ELEMENT_H_
