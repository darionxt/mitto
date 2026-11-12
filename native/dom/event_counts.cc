#include "mitto/dom/event_counts.h"

#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/runtime_features.h"
#include "mitto/bindings/v8_event_counts.h"
#include "v8/include/v8-isolate.h"

namespace mitto {
namespace {

// The types that EventCounts::EventCounts() counts, in the order in which
// Chrome 157's HashMap of them gives them (Mac Chrome 157.0.8103.51,
// 2026-11-13), which follows how WTF hashes the names.
constexpr const char16_t* kEventTypes[] = {
    u"pointerdown",
    u"touchend",
    u"input",
    u"keydown",
    u"mouseleave",
    u"mouseenter",
    u"drop",
    u"beforeinput",
    u"pointerenter",
    u"dragend",
    u"pointercancel",
    u"compositionupdate",
    u"mousedown",
    u"dragleave",
    u"dragover",
    u"mouseup",
    u"pointerover",
    u"lostpointercapture",
    u"mouseover",
    u"gotpointercapture",
    u"dblclick",
    u"keyup",
    u"keypress",
    u"pointerup",
    u"compositionstart",
    u"auxclick",
    u"dragstart",
    u"touchstart",
    u"compositionend",
    u"pointerout",
    u"dragenter",
    u"touchcancel",
    u"click",
    u"contextmenu",
    u"mouseout",
    u"pointerleave",
};

}  // namespace

// static
EventCounts* EventCounts::Create() {
  return bindings::MakeGarbageCollected<EventCounts>();
}

// static
bindings::ScriptWrappable* EventCounts::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<EventCounts>(
      info, bindings::V8EventCounts::kWrapperTypeInfo);
}

EventCounts::EventCounts() = default;
EventCounts::~EventCounts() = default;

const bindings::WrapperTypeInfo* EventCounts::GetWrapperTypeInfo() const {
  return &bindings::V8EventCounts::kWrapperTypeInfo;
}

std::vector<std::pair<std::u16string, uint64_t>> EventCounts::MaplikeEntries()
    const {
  std::vector<std::pair<std::u16string, uint64_t>> entries;
  for (const char16_t* type : kEventTypes) {
    entries.emplace_back(type, 0);
  }
  // Blink adds these two behind NavigationEventTiming, which the stable set
  // lacks. Where they go in the HashMap's order is not known: at the end.
  if (bindings::PerIsolateData::From(v8::Isolate::GetCurrent())
          ->features()
          .IsEnabled(bindings::RuntimeFeature::kNavigationEventTiming)) {
    entries.emplace_back(u"navigate", 0);
    entries.emplace_back(u"popstate", 0);
  }
  return entries;
}

}  // namespace mitto
