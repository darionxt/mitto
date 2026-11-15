#include "mitto/dom/event_factory.h"

#include <string>
#include <string_view>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/string_util.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/interfaces_without_class.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/wrapper_type_info.h"
#include "mitto/dom/before_unload_event.h"
#include "mitto/dom/custom_event.h"
#include "mitto/dom/event.h"
#include "mitto/dom/hash_change_event.h"
#include "mitto/dom/message_event.h"
#include "mitto/dom/objects_behind.h"
#include "mitto/dom/storage_event.h"

namespace mitto {

namespace {

// An interface that the factories take, by one of its names: one with a class
// makes its event with `create`, and one without is an Event behind a wrapper
// of `interface`.
struct EventInterfaceName {
  std::u16string_view name;
  Event* (*create)();
  // RAW_PTR_EXCLUSION: #global-scope, as in WrapperTypeInfo.
  RAW_PTR_EXCLUSION const bindings::WrapperTypeInfo* interface;
};

Event* CreateEvent() {
  return Event::Create(u"", bindings::EventInit());
}

Event* CreateCustomEvent() {
  return CustomEvent::Create(u"", bindings::CustomEventInit());
}

Event* CreateMessageEvent() {
  return bindings::MakeGarbageCollected<MessageEvent>(
      u"", bindings::MessageEventInit());
}

Event* CreateHashChangeEvent() {
  return HashChangeEvent::Create(u"", bindings::HashChangeEventInit());
}

Event* CreateStorageEvent() {
  return StorageEvent::Create(u"", bindings::StorageEventInit());
}

Event* CreateBeforeUnloadEvent() {
  return bindings::MakeGarbageCollected<BeforeUnloadEvent>(
      u"", bindings::EventInit());
}

// The names of make_event_factory.py's four lists that name an event
// interface of core or modules, or an alias of event_interface_aliases.json5,
// with RemoveNonAllowlistedCreateEvent stable in the Engine Release, which
// leaves out the list of create_event_removed_by_flag(): AnimationEvent,
// CloseEvent, ErrorEvent, IDBVersionChangeEvent, PageTransitionEvent,
// PopStateEvent, TrackEvent, WebGLContextEvent and WheelEvent. None of them
// is behind a runtime-enabled feature.
constexpr EventInterfaceName kEventInterfaceNames[] = {
    // create_event_ignore_case_list()
    {u"Event", &CreateEvent, nullptr},
    {u"Events", &CreateEvent, nullptr},
    {u"HTMLEvents", &CreateEvent, nullptr},
    {u"UIEvent", nullptr, &bindings::V8UIEvent::kWrapperTypeInfo},
    {u"UIEvents", nullptr, &bindings::V8UIEvent::kWrapperTypeInfo},
    {u"CustomEvent", &CreateCustomEvent, nullptr},
    {u"KeyboardEvent", nullptr, &bindings::V8KeyboardEvent::kWrapperTypeInfo},
    {u"MessageEvent", &CreateMessageEvent, nullptr},
    {u"MouseEvent", nullptr, &bindings::V8MouseEvent::kWrapperTypeInfo},
    {u"MouseEvents", nullptr, &bindings::V8MouseEvent::kWrapperTypeInfo},
    {u"TouchEvent", nullptr, &bindings::V8TouchEvent::kWrapperTypeInfo},
    // create_event_ignore_case_and_measure_list()
    {u"BeforeUnloadEvent", &CreateBeforeUnloadEvent, nullptr},
    {u"CompositionEvent", nullptr,
     &bindings::V8CompositionEvent::kWrapperTypeInfo},
    {u"DeviceMotionEvent", nullptr,
     &bindings::V8DeviceMotionEvent::kWrapperTypeInfo},
    {u"DeviceOrientationEvent", nullptr,
     &bindings::V8DeviceOrientationEvent::kWrapperTypeInfo},
    {u"DragEvent", nullptr, &bindings::V8DragEvent::kWrapperTypeInfo},
    {u"FocusEvent", nullptr, &bindings::V8FocusEvent::kWrapperTypeInfo},
    {u"HashChangeEvent", &CreateHashChangeEvent, nullptr},
    {u"StorageEvent", &CreateStorageEvent, nullptr},
    {u"SVGEvents", &CreateEvent, nullptr},
    {u"TextEvent", nullptr, &bindings::V8TextEvent::kWrapperTypeInfo},
    // create_event_deprecated()
    {u"KeyboardEvents", nullptr, &bindings::V8KeyboardEvent::kWrapperTypeInfo},
    {u"TransitionEvent", nullptr,
     &bindings::V8TransitionEvent::kWrapperTypeInfo},
};

}  // namespace

Event* CreateEventOfInterfaceName(const std::u16string& name) {
  for (const EventInterfaceName& entry : kEventInterfaceNames) {
    if (!base::EqualsCaseInsensitiveASCII(name, entry.name)) {
      continue;
    }
    Event* const event = entry.create
                             ? entry.create()
                             : CreateEventOfInterface(*entry.interface, u"",
                                                      bindings::EventInit());
    event->SetUninitialized();
    return event;
  }
  return nullptr;
}

}  // namespace mitto
