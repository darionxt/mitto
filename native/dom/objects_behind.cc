#include "mitto/dom/objects_behind.h"

#include <string>

#include "base/memory/raw_ptr.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/interfaces_without_class.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/wrapper_type_info.h"
#include "mitto/dom/event.h"
#include "mitto/dom/event_target.h"

namespace mitto {

namespace {

class EventTargetOfInterface final : public EventTarget {
 public:
  explicit EventTargetOfInterface(const bindings::WrapperTypeInfo& info)
      : info_(&info) {}

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override {
    return info_;
  }

 private:
  const raw_ptr<const bindings::WrapperTypeInfo> info_;
};

class EventOfInterface final : public Event {
 public:
  EventOfInterface(const bindings::WrapperTypeInfo& info,
                   const std::u16string& type,
                   const bindings::EventInit& init)
      : Event(type, init), info_(&info) {}

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override {
    return info_;
  }

  // Event:
  bool IsMouseEvent() const override {
    for (const bindings::WrapperTypeInfo* info = info_; info;
         info = info->parent) {
      if (info == &bindings::V8MouseEvent::kWrapperTypeInfo) {
        return true;
      }
    }
    return false;
  }

 private:
  const raw_ptr<const bindings::WrapperTypeInfo> info_;
};

bindings::ScriptWrappable* CreateEventTarget(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeGarbageCollected<EventTargetOfInterface>(info);
}

bindings::ScriptWrappable* CreateEvent(const bindings::WrapperTypeInfo& info,
                                       const std::u16string& type,
                                       bool bubbles,
                                       bool cancelable,
                                       bool composed) {
  bindings::EventInit init;
  init.bubbles = bubbles;
  init.cancelable = cancelable;
  init.composed = composed;
  return bindings::MakeGarbageCollected<EventOfInterface>(info, type, init);
}

}  // namespace

bindings::PerIsolateData::ObjectHooks ObjectsBehindHooks() {
  return {&CreateEventTarget, &CreateEvent};
}

Event* CreateEventOfInterface(const bindings::WrapperTypeInfo& info,
                              const std::u16string& type,
                              const bindings::EventInit& init) {
  return bindings::MakeGarbageCollected<EventOfInterface>(info, type, init);
}

}  // namespace mitto
