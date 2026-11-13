#include "mitto/dom/performance_entry.h"

#include <array>
#include <utility>

#include "base/atomic_sequence_num.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_performance_entry.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

// Blink numbers the entries of a process in the order they are made.
base::AtomicSequenceNumber g_index;

// performance_entry_names.json5, with the type each name stands for.
constexpr std::array<std::pair<std::u16string_view, PerformanceEntry::Kind>, 19>
    kEntryTypes = {{
        {u"longtask", PerformanceEntry::kLongTask},
        {u"mark", PerformanceEntry::kMark},
        {u"measure", PerformanceEntry::kMeasure},
        {u"resource", PerformanceEntry::kResource},
        {u"navigation", PerformanceEntry::kNavigation},
        {u"taskattribution", PerformanceEntry::kTaskAttribution},
        {u"paint", PerformanceEntry::kPaint},
        {u"event", PerformanceEntry::kEvent},
        {u"first-input", PerformanceEntry::kFirstInput},
        {u"element", PerformanceEntry::kElement},
        {u"layout-shift", PerformanceEntry::kLayoutShift},
        {u"largest-contentful-paint",
         PerformanceEntry::kLargestContentfulPaint},
        {u"interaction-contentful-paint",
         PerformanceEntry::kInteractionContentfulPaint},
        {u"visibility-state", PerformanceEntry::kVisibilityState},
        {u"back-forward-cache-restoration",
         PerformanceEntry::kBackForwardCacheRestoration},
        {u"soft-navigation", PerformanceEntry::kSoftNavigation},
        {u"long-animation-frame", PerformanceEntry::kLongAnimationFrame},
        {u"container", PerformanceEntry::kContainer},
        {u"scroll", PerformanceEntry::kScroll},
    }};

}  // namespace

// static
bindings::ScriptWrappable* PerformanceEntry::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<PerformanceEntry>(
      info, bindings::V8PerformanceEntry::kWrapperTypeInfo, std::u16string(),
      0.0, 0.0, uint64_t{0});
}

PerformanceEntry::PerformanceEntry(std::u16string name,
                                   double start_time,
                                   double finish_time,
                                   uint64_t navigation_id)
    : duration_(finish_time - start_time),
      name_(std::move(name)),
      start_time_(start_time),
      index_(static_cast<uint64_t>(g_index.GetNext())),
      navigation_id_(navigation_id) {}

PerformanceEntry::~PerformanceEntry() = default;

const bindings::WrapperTypeInfo* PerformanceEntry::GetWrapperTypeInfo() const {
  return &bindings::V8PerformanceEntry::kWrapperTypeInfo;
}

double PerformanceEntry::Duration() const {
  return duration_;
}

PerformanceEntry::Kind PerformanceEntry::GetKind() const {
  return kInvalid;
}

std::u16string_view PerformanceEntry::TypeName() const {
  return u"";
}

v8::Local<v8::Object> PerformanceEntry::ToJSONForBinding() const {
  V8ObjectBuilder builder(v8::Isolate::GetCurrent());
  BuildJSONValue(builder);
  return builder.Result();
}

void PerformanceEntry::BuildJSONValue(V8ObjectBuilder& builder) const {
  builder.AddString("name", Name());
  builder.AddString("entryType", TypeName());
  builder.AddNumber("startTime", StartTime());
  builder.AddNumber("duration", Duration());
  builder.AddNumber("navigationId", static_cast<double>(NavigationId()));
}

// static
PerformanceEntry::Kind PerformanceEntry::ToKind(
    std::u16string_view entry_type) {
  for (const auto& [name, type] : kEntryTypes) {
    if (name == entry_type) {
      return type;
    }
  }
  return kInvalid;
}

// static
bool PerformanceEntry::IsValidTimelineEntryKind(Kind kind) {
  if (kind == kInvalid) {
    return true;
  }
  constexpr uint32_t kTimelineEntryMask =
      kNavigation | kMark | kMeasure | kResource | kTaskAttribution | kPaint |
      kFirstInput | kBackForwardCacheRestoration | kSoftNavigation |
      kLongAnimationFrame | kVisibilityState;
  return (kind & kTimelineEntryMask) != 0;
}

// static
bool PerformanceEntry::StartTimeCompareLessThan(const PerformanceEntry* a,
                                                const PerformanceEntry* b) {
  if (a->StartTime() == b->StartTime()) {
    // Blink makes the navigation's entry lazily, and keeps it first.
    if (a->GetKind() == kNavigation && b->GetKind() != kNavigation) {
      return true;
    }
    if (b->GetKind() == kNavigation) {
      return false;
    }
    if (a->GetKind() == kPaint && b->GetKind() != kPaint) {
      return true;
    }
    if (b->GetKind() == kPaint) {
      return false;
    }
    return a->index_ < b->index_;
  }
  return a->StartTime() < b->StartTime();
}

V8ObjectBuilder::V8ObjectBuilder(v8::Isolate* isolate)
    : isolate_(isolate), object_(v8::Object::New(isolate)) {}

V8ObjectBuilder::~V8ObjectBuilder() = default;

void V8ObjectBuilder::AddNumber(std::string_view name, double value) {
  Add(name, v8::Number::New(isolate_, value));
}

void V8ObjectBuilder::AddString(std::string_view name,
                                std::u16string_view value) {
  Add(name, bindings::ToV8String(isolate_, value));
}

void V8ObjectBuilder::AddNull(std::string_view name) {
  Add(name, v8::Null(isolate_));
}

void V8ObjectBuilder::Add(std::string_view name, v8::Local<v8::Value> value) {
  // V8ObjectBuilder::AddInternal() stops at the first property that cannot be
  // defined, which on a new plain object only termination brings.
  if (failed_) {
    return;
  }
  failed_ = value.IsEmpty() ||
            object_
                ->CreateDataProperty(
                    isolate_->GetCurrentContext(),
                    bindings::ToV8StringFromUtf8(isolate_, name), value)
                .IsNothing();
}

v8::Local<v8::Object> V8ObjectBuilder::Result() const {
  return object_;
}

}  // namespace mitto
