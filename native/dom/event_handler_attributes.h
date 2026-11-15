// The event handler content attributes of elements, such as onclick, as
// Blink's JSEventHandlerForContentAttribute and the attribute tables of its
// elements have them: setting one gives the element, or for some of a body's
// and a frameset's its window, a handler that is compiled from the
// attribute's text the first time it is needed, with the element, its form
// owner and its document as scopes.
//
// Upstream: third_party/blink/renderer/bindings/core/v8/js_event_handler_for_content_attribute.cc
// Upstream: third_party/blink/renderer/core/html/html_element.cc
// Upstream: third_party/blink/renderer/core/html/html_body_element.cc
// Upstream: third_party/blink/renderer/core/html/html_frame_set_element.cc

#ifndef MITTO_DOM_EVENT_HANDLER_ATTRIBUTES_H_
#define MITTO_DOM_EVENT_HANDLER_ATTRIBUTES_H_

#include <string>
#include <string_view>

#include "mitto/dom/markup_position.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"

namespace mitto {

class Element;
class EventTarget;
struct QualifiedName;

// The HTML Standard's internal raw uncompiled handler: what an event handler
// content attribute set, until it is compiled.
class RawEventHandler final : public cppgc::GarbageCollected<RawEventHandler> {
 public:
  // The text of the attribute `function_name`, from the document at
  // `source_url`, where it was at `position`. `on_error` for an onerror
  // attribute, whose handler on a window takes the five arguments of the
  // special error event handling.
  RawEventHandler(std::u16string function_name,
                  std::u16string body,
                  std::u16string source_url,
                  MarkupPosition position,
                  bool on_error);
  ~RawEventHandler();

  void Trace(cppgc::Visitor* visitor) const {}

  // Whether it has been compiled, or has failed to.
  bool did_compile() const { return did_compile_; }

  // Compiles it for `target`, the element or window whose handler it is, the
  // first time it is needed: the function, or null if the target's document
  // has no window or the text does not parse as a function body, which is
  // reported as an uncaught error.
  v8::Local<v8::Value> Compile(v8::Isolate* isolate, EventTarget& target);

 private:
  const std::u16string function_name_;
  const std::u16string body_;
  const std::u16string source_url_;
  const MarkupPosition position_;
  const bool on_error_;
  bool did_compile_ = false;
};

// The event type that the content attribute `name` of an HTML, SVG or MathML
// element sets a handler for, as Blink's
// HTMLElement::EventNameForAttributeName() gives it, or empty for one that is
// not an event handler content attribute.
std::u16string_view EventNameForAttributeName(const QualifiedName& name);

// The event type of the window's handler that the content attribute `name` of
// a body or a frameset element sets, or empty.
std::u16string_view BodyWindowEventNameForAttributeName(
    const QualifiedName& name);
std::u16string_view FrameSetWindowEventNameForAttributeName(
    const QualifiedName& name);

// Blink's Element::SetElementAttributeEventListenerFromScriptBody() and
// SetWindowAttributeEventListenerFromScriptBody(): the handler for
// `event_type` of `element`, or of its document's window, from the attribute
// `name` that changed to `value`, which a null value removes.
void SetElementAttributeEventListenerFromScriptBody(
    Element& element,
    std::u16string_view event_type,
    const QualifiedName& name,
    const std::u16string* value);
void SetWindowAttributeEventListenerFromScriptBody(
    Element& element,
    std::u16string_view event_type,
    const QualifiedName& name,
    const std::u16string* value);

}  // namespace mitto

#endif  // MITTO_DOM_EVENT_HANDLER_ATTRIBUTES_H_
