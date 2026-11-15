#include "mitto/dom/event_handler_attributes.h"

#include <array>
#include <utility>

#include "base/check.h"
#include "base/numerics/safe_conversions.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_window.h"
#include "mitto/dom/document.h"
#include "mitto/dom/element.h"
#include "mitto/dom/error_reporting.h"
#include "mitto/dom/event_target.h"
#include "mitto/dom/html_element.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/node.h"
#include "mitto/dom/qualified_name.h"
#include "mitto/dom/window.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-script.h"

namespace mitto {

namespace {

struct AttributeEvent {
  std::u16string_view attribute;
  std::u16string_view event;
};

// Blink's AttributeTriggers of HTMLElement that name an event.
constexpr AttributeEvent kElementEvents[] = {
    {u"onabort", u"abort"},
    {u"onanimationcancel", u"animationcancel"},
    {u"onanimationend", u"animationend"},
    {u"onanimationiteration", u"animationiteration"},
    {u"onanimationstart", u"animationstart"},
    {u"onauxclick", u"auxclick"},
    {u"onbeforecopy", u"beforecopy"},
    {u"onbeforecut", u"beforecut"},
    {u"onbeforefilter", u"beforefilter"},
    {u"onbeforeinput", u"beforeinput"},
    {u"onbeforepaste", u"beforepaste"},
    {u"onbeforetoggle", u"beforetoggle"},
    {u"onblur", u"blur"},
    {u"oncancel", u"cancel"},
    {u"oncanplay", u"canplay"},
    {u"oncanplaythrough", u"canplaythrough"},
    {u"onchange", u"change"},
    {u"onclick", u"click"},
    {u"onclose", u"close"},
    {u"oncommand", u"command"},
    {u"oncontentvisibilityautostatechange",
     u"contentvisibilityautostatechange"},
    {u"oncontextlost", u"contextlost"},
    {u"oncontextmenu", u"contextmenu"},
    {u"oncontextrestored", u"contextrestored"},
    {u"oncopy", u"copy"},
    {u"oncuechange", u"cuechange"},
    {u"oncut", u"cut"},
    {u"ondblclick", u"dblclick"},
    {u"ondrag", u"drag"},
    {u"ondragend", u"dragend"},
    {u"ondragenter", u"dragenter"},
    {u"ondragleave", u"dragleave"},
    {u"ondragover", u"dragover"},
    {u"ondragstart", u"dragstart"},
    {u"ondrop", u"drop"},
    {u"ondurationchange", u"durationchange"},
    {u"onemptied", u"emptied"},
    {u"onended", u"ended"},
    {u"onerror", u"error"},
    {u"onfocus", u"focus"},
    {u"onfocusin", u"focusin"},
    {u"onfocusout", u"focusout"},
    {u"onformdata", u"formdata"},
    {u"ongotpointercapture", u"gotpointercapture"},
    {u"oninput", u"input"},
    {u"oninstallresult", u"installresult"},
    {u"oninvalid", u"invalid"},
    {u"onkeydown", u"keydown"},
    {u"onkeypress", u"keypress"},
    {u"onkeyup", u"keyup"},
    {u"onload", u"load"},
    {u"onloadeddata", u"loadeddata"},
    {u"onloadedmetadata", u"loadedmetadata"},
    {u"onloadstart", u"loadstart"},
    {u"onlostpointercapture", u"lostpointercapture"},
    {u"onmousedown", u"mousedown"},
    {u"onmouseenter", u"mouseenter"},
    {u"onmouseleave", u"mouseleave"},
    {u"onmousemove", u"mousemove"},
    {u"onmouseout", u"mouseout"},
    {u"onmouseover", u"mouseover"},
    {u"onmouseup", u"mouseup"},
    {u"onmousewheel", u"mousewheel"},
    {u"onpaste", u"paste"},
    {u"onpause", u"pause"},
    {u"onplay", u"play"},
    {u"onplaying", u"playing"},
    {u"onpointercancel", u"pointercancel"},
    {u"onpointerdown", u"pointerdown"},
    {u"onpointerenter", u"pointerenter"},
    {u"onpointerleave", u"pointerleave"},
    {u"onpointermove", u"pointermove"},
    {u"onpointerout", u"pointerout"},
    {u"onpointerover", u"pointerover"},
    {u"onpointerrawupdate", u"pointerrawupdate"},
    {u"onpointerup", u"pointerup"},
    {u"onprogress", u"progress"},
    {u"onpromptaction", u"promptaction"},
    {u"onpromptdismiss", u"promptdismiss"},
    {u"onratechange", u"ratechange"},
    {u"onreset", u"reset"},
    {u"onresize", u"resize"},
    {u"onscroll", u"scroll"},
    {u"onscrollend", u"scrollend"},
    {u"onseeked", u"seeked"},
    {u"onseeking", u"seeking"},
    {u"onsecuritypolicyviolation", u"securitypolicyviolation"},
    {u"onselect", u"select"},
    {u"onselectionchange", u"selectionchange"},
    {u"onselectstart", u"selectstart"},
    {u"onslotchange", u"slotchange"},
    {u"onscrollsnapchange", u"scrollsnapchange"},
    {u"onscrollsnapchanging", u"scrollsnapchanging"},
    {u"onstalled", u"stalled"},
    {u"onstream", u"stream"},
    {u"onsubmit", u"submit"},
    {u"onsuspend", u"suspend"},
    {u"ontimeupdate", u"timeupdate"},
    {u"ontoggle", u"toggle"},
    {u"ontouchcancel", u"touchcancel"},
    {u"ontouchend", u"touchend"},
    {u"ontouchmove", u"touchmove"},
    {u"ontouchstart", u"touchstart"},
    {u"ontransitionend", u"webkitTransitionEnd"},
    {u"onvalidationstatuschange", u"validationstatuschange"},
    {u"onvolumechange", u"volumechange"},
    {u"onwaiting", u"waiting"},
    {u"onwebkitanimationend", u"webkitAnimationEnd"},
    {u"onwebkitanimationiteration", u"webkitAnimationIteration"},
    {u"onwebkitanimationstart", u"webkitAnimationStart"},
    {u"onwebkitfullscreenchange", u"webkitfullscreenchange"},
    {u"onwebkitfullscreenerror", u"webkitfullscreenerror"},
    {u"onwebkittransitionend", u"webkitTransitionEnd"},
    {u"onwheel", u"wheel"},
    {u"onlocation", u"location"},
};

// The window's handlers that html_body_element's ParseAttribute() sets.
constexpr AttributeEvent kBodyWindowEvents[] = {
    {u"onafterprint", u"afterprint"},
    {u"onbeforeprint", u"beforeprint"},
    {u"onload", u"load"},
    {u"onbeforeunload", u"beforeunload"},
    {u"onunload", u"unload"},
    {u"onpagehide", u"pagehide"},
    {u"onpageshow", u"pageshow"},
    {u"onpopstate", u"popstate"},
    {u"onblur", u"blur"},
    {u"onerror", u"error"},
    {u"onfocus", u"focus"},
    {u"onorientationchange", u"orientationchange"},
    {u"onhashchange", u"hashchange"},
    {u"onmessage", u"message"},
    {u"onmessageerror", u"messageerror"},
    {u"onresize", u"resize"},
    {u"onscroll", u"scroll"},
    {u"onstorage", u"storage"},
    {u"ononline", u"online"},
    {u"onoffline", u"offline"},
    {u"onlanguagechange", u"languagechange"},
    {u"ontimezonechange", u"timezonechange"},
};

// The window's handlers that html_frame_set_element's ParseAttribute() sets.
constexpr AttributeEvent kFrameSetWindowEvents[] = {
    {u"onafterprint", u"afterprint"},
    {u"onbeforeprint", u"beforeprint"},
    {u"onload", u"load"},
    {u"onbeforeunload", u"beforeunload"},
    {u"onunload", u"unload"},
    {u"onpagehide", u"pagehide"},
    {u"onpageshow", u"pageshow"},
    {u"onblur", u"blur"},
    {u"onerror", u"error"},
    {u"onfocus", u"focus"},
    {u"onfocusin", u"focusin"},
    {u"onfocusout", u"focusout"},
    {u"onorientationchange", u"orientationchange"},
    {u"onhashchange", u"hashchange"},
    {u"onmessage", u"message"},
    {u"onresize", u"resize"},
    {u"onscroll", u"scroll"},
    {u"onstorage", u"storage"},
    {u"ononline", u"online"},
    {u"onoffline", u"offline"},
    {u"onpopstate", u"popstate"},
    {u"onlanguagechange", u"languagechange"},
    {u"ontimezonechange", u"timezonechange"},
};

template <size_t N>
std::u16string_view Find(const AttributeEvent (&table)[N],
                         const QualifiedName& name) {
  // Each is an attribute of no namespace.
  if (name.namespace_uri || !name.local_name.starts_with(u"on")) {
    return {};
  }
  for (const AttributeEvent& entry : table) {
    if (entry.attribute == name.local_name) {
      return entry.event;
    }
  }
  return {};
}

// The window that runs the element's handlers, Blink's execution context of
// the element: its document's, or for a document without one, such as one
// that createHTMLDocument() made, the window of the script that changes it,
// as Blink gives such a document its creator's.
Window* ContextWindowOf(Element& element) {
  if (Window* const window = element.GetDocument().DefaultView()) {
    return window;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (!isolate || !isolate->InContext()) {
    return nullptr;
  }
  return bindings::V8Window::ToWrappable(
      isolate, isolate->GetCurrentContext()->Global());
}

// JSEventHandlerForContentAttribute::Create(): none for a removed attribute,
// or without a window to run it in.
RawEventHandler* CreateRawHandler(Element& element,
                                  const QualifiedName& name,
                                  const std::u16string* value) {
  if (!value) {
    return nullptr;
  }
  Window* const window = ContextWindowOf(element);
  if (!window || !window->HasContext()) {
    return nullptr;
  }
  // The URL of the window's document, and where its parser is, if it is
  // parsing, as Blink's ScriptController::EventHandlerPosition() gives it.
  Document& document = window->GetDocument();
  return bindings::MakeGarbageCollected<RawEventHandler>(
      name.local_name, *value, document.UrlForBinding(),
      document.EventHandlerPosition(), name.local_name == u"onerror");
}

v8::Local<v8::String> V8String(v8::Isolate* isolate, std::u16string_view text) {
  return bindings::ToV8String(isolate, text);
}

}  // namespace

RawEventHandler::RawEventHandler(std::u16string function_name,
                                 std::u16string body,
                                 std::u16string source_url,
                                 MarkupPosition position,
                                 bool on_error)
    : function_name_(std::move(function_name)),
      body_(std::move(body)),
      source_url_(std::move(source_url)),
      position_(position),
      on_error_(on_error) {}

RawEventHandler::~RawEventHandler() = default;

v8::Local<v8::Value> RawEventHandler::Compile(v8::Isolate* isolate,
                                              EventTarget& target) {
  // Blink's JSEventHandlerForContentAttribute::GetCompiledHandler(), which
  // compiles the same text once.
  DCHECK(!did_compile_);
  did_compile_ = true;
  // Step 1: the element and its document, or the window and its.
  Element* element = nullptr;
  Window* window = nullptr;
  Document* document = nullptr;
  if (Node* const node = target.ToNode()) {
    document = &node->GetDocument();
    if (node->IsElementNode()) {
      element = static_cast<Element*>(node);
    }
  } else {
    window = target.ToWindow();
    if (!window) {
      return v8::Null(isolate);
    }
    document = &window->GetDocument();
  }
  // Step 2, as Blink's Document::AllowInlineEventHandler(): a document without
  // a window runs no inline script.
  Window* const document_window = document->DefaultView();
  if (!document_window || !document_window->HasContext()) {
    return v8::Null(isolate);
  }
  v8::Local<v8::Context> context;
  if (!document_window->ToV8(isolate)->GetCreationContext(isolate).ToLocal(
          &context)) {
    return v8::Null(isolate);
  }
  const v8::Context::Scope context_scope(context);
  // Step 5: the form owner. A body's and a frameset's handlers are treated as
  // the window's.
  Element* form_owner = nullptr;
  if (element && element->IsHTMLElement()) {
    auto* const html_element = static_cast<HTMLElement*>(element);
    form_owner = html_element->FormOwner();
    if (html_element->HasHTMLTagName(u"body") ||
        html_element->HasHTMLTagName(u"frameset")) {
      window = document_window;
    }
  }
  // Step 10: one argument, which SVG also calls evt, or the five of a
  // window's onerror; the element, its form owner and its document as scopes.
  const bool svg =
      element && element->NamespaceURI() == html_names::kSvgNamespace;
  std::array<v8::Local<v8::String>, 5> parameters;
  size_t parameter_count = 0;
  parameters[parameter_count++] = V8String(isolate, svg ? u"evt" : u"event");
  if (on_error_ && window) {
    parameters[parameter_count++] = V8String(isolate, u"source");
    parameters[parameter_count++] = V8String(isolate, u"lineno");
    parameters[parameter_count++] = V8String(isolate, u"colno");
    parameters[parameter_count++] = V8String(isolate, u"error");
  }
  std::array<v8::Local<v8::Object>, 3> scopes;
  size_t scope_count = 0;
  if (element) {
    scopes[scope_count++] = bindings::ToV8(isolate, document).As<v8::Object>();
  }
  if (form_owner) {
    scopes[scope_count++] =
        bindings::ToV8(isolate, form_owner).As<v8::Object>();
  }
  if (element) {
    scopes[scope_count++] = bindings::ToV8(isolate, element).As<v8::Object>();
  }
  v8::ScriptOrigin origin = ClassicScriptOrigin(
      V8String(isolate, source_url_),
      /*muted_errors=*/false, position_.line, position_.column);
  v8::ScriptCompiler::Source source(V8String(isolate, body_), origin);
  v8::Local<v8::Function> function;
  {
    // Step 7: a body that does not parse is reported, and gives null.
    const v8::TryCatch try_catch(isolate);
    if (!v8::ScriptCompiler::CompileFunction(context, &source, parameter_count,
                                             parameters.data(), scope_count,
                                             scopes.data())
             .ToLocal(&function)) {
      ReportScriptException(isolate, try_catch);
      return v8::Null(isolate);
    }
  }
  function->SetName(V8String(isolate, function_name_));
  return function;
}

std::u16string_view EventNameForAttributeName(const QualifiedName& name) {
  return Find(kElementEvents, name);
}

std::u16string_view BodyWindowEventNameForAttributeName(
    const QualifiedName& name) {
  return Find(kBodyWindowEvents, name);
}

std::u16string_view FrameSetWindowEventNameForAttributeName(
    const QualifiedName& name) {
  return Find(kFrameSetWindowEvents, name);
}

void SetElementAttributeEventListenerFromScriptBody(
    Element& element,
    std::u16string_view event_type,
    const QualifiedName& name,
    const std::u16string* value) {
  element.SetAttributeEventListener(std::u16string(event_type),
                                    CreateRawHandler(element, name, value));
}

void SetWindowAttributeEventListenerFromScriptBody(
    Element& element,
    std::u16string_view event_type,
    const QualifiedName& name,
    const std::u16string* value) {
  RawEventHandler* const handler = CreateRawHandler(element, name, value);
  // Blink's Document::SetWindowAttributeEventListener(): nothing for a
  // document without a window.
  if (Window* const window = element.GetDocument().DefaultView()) {
    window->SetAttributeEventListener(std::u16string(event_type), handler);
  }
}

}  // namespace mitto
