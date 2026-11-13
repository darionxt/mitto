#include "mitto/dom/event_handlers.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>

#include "mitto/bindings/v8_event_target.h"
#include "mitto/bindings/v8_html_body_element.h"
#include "mitto/bindings/v8_message_port.h"
#include "mitto/dom/document.h"
#include "mitto/dom/event_target.h"
#include "mitto/dom/html_body_element.h"
#include "mitto/dom/message_port.h"
#include "mitto/dom/window.h"
#include "v8/include/v8-primitive.h"

// Upstream: third_party/blink/renderer/core/dom/events/event_target.h
// Upstream: third_party/blink/renderer/core/html/html_body_element.h
// Upstream: third_party/blink/renderer/core/frame/window_event_handlers.h

namespace mitto {

namespace {

// The events whose handlers an HTMLBodyElement takes from its window: those of
// WindowEventHandlers, and the window's own of GlobalEventHandlers, as Blink's
// DEFINE_WINDOW_ATTRIBUTE_EVENT_LISTENER lines have them.
constexpr std::u16string_view kWindowEvents[] = {
    u"afterprint",
    u"beforeprint",
    u"beforeunload",
    u"blur",
    u"error",
    u"focus",
    u"hashchange",
    u"languagechange",
    u"load",
    u"message",
    u"messageerror",
    u"move",
    u"offline",
    u"online",
    u"openerchange",
    u"orientationchange",
    u"pagehide",
    u"pageshow",
    u"popstate",
    u"rejectionhandled",
    u"resize",
    u"scroll",
    u"storage",
    u"timezonechange",
    u"unhandledrejection",
    u"unload",
};

// What the receiver's handler for `type` belongs to: the receiver, or for an
// HTMLBodyElement and a window's event, its window, which may be none.
// Returns false for a receiver that is not an EventTarget.
bool HandlerTarget(v8::Isolate* isolate,
                   v8::Local<v8::Object> receiver,
                   const std::u16string& type,
                   EventTarget** target) {
  EventTarget* const event_target =
      bindings::V8EventTarget::ToWrappable(isolate, receiver);
  if (!event_target) {
    return false;
  }
  *target = event_target;
  HTMLBodyElement* const body =
      bindings::V8HTMLBodyElement::ToWrappable(isolate, receiver);
  if (body && std::find(std::begin(kWindowEvents), std::end(kWindowEvents),
                        type) != std::end(kWindowEvents)) {
    *target = body->GetDocument().DefaultView();
  }
  return true;
}

bool GetEventHandler(v8::Isolate* isolate,
                     v8::Local<v8::Object> receiver,
                     const std::u16string& type,
                     v8::Local<v8::Value>* handler) {
  EventTarget* target = nullptr;
  if (!HandlerTarget(isolate, receiver, type, &target)) {
    return false;
  }
  *handler = target ? target->AttributeEventHandler(isolate, type)
                    : v8::Null(isolate).As<v8::Value>();
  return true;
}

bool SetEventHandler(v8::Isolate* isolate,
                     v8::Local<v8::Object> receiver,
                     const std::u16string& type,
                     v8::Local<v8::Value> handler) {
  EventTarget* target = nullptr;
  if (!HandlerTarget(isolate, receiver, type, &target)) {
    return false;
  }
  if (target) {
    target->SetAttributeEventHandler(isolate, type, handler);
  }
  // MessagePort's setters of onmessage and onmessageerror start the port,
  // whatever they are set to, as Blink's do.
  if (type == u"message" || type == u"messageerror") {
    if (MessagePort* const port =
            bindings::V8MessagePort::ToWrappable(isolate, receiver)) {
      port->Start();
    }
  }
  return true;
}

}  // namespace

void AddEventHandlerHooks(bindings::PerIsolateData::ObjectHooks& hooks) {
  hooks.get_event_handler = &GetEventHandler;
  hooks.set_event_handler = &SetEventHandler;
}

}  // namespace mitto
