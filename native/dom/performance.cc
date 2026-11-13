#include "mitto/dom/performance.h"

#include <utility>
#include <variant>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/no_destructor.h"
#include "base/rand_util.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_performance.h"
#include "mitto/bindings/v8_window.h"
#include "mitto/dom/event.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/memory_info.h"
#include "mitto/dom/performance_entry.h"
#include "mitto/dom/performance_mark.h"
#include "mitto/dom/performance_measure.h"
#include "mitto/dom/performance_navigation.h"
#include "mitto/dom/performance_navigation_timing.h"
#include "mitto/dom/performance_resource_timing.h"
#include "mitto/dom/performance_timing.h"
#include "mitto/dom/serialized_script_value.h"
#include "mitto/dom/time_clamper.h"
#include "mitto/dom/visibility_state_entry.h"
#include "mitto/dom/window.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-persistent-handle.h"

namespace mitto {
namespace {

// A Page is never cross-origin isolated.
constexpr bool kCrossOriginIsolated = false;

// Blink's kDefaultResourceTimingBufferSize.
constexpr uint32_t kDefaultResourceTimingBufferSize = 250;

// PerformanceTimelineEntryIdInfo's kMinId and kMaxIdForReset.
constexpr int kMinNavigationId = 100;
constexpr int kMaxNavigationIdForReset = 10000;

// Blink's GetUnixAtZeroMonotonic().
base::Time UnixAtZeroMonotonic() {
  return base::Time::Now() - (base::TimeTicks::Now() - base::TimeTicks());
}

bool HasName(const PerformanceEntry* entry,
             const std::optional<std::u16string>& name) {
  return !name || entry->Name() == *name;
}

// MergePerformanceEntryVectors(): the two in the timeline's order, the first
// winning ties, of the second only those named `name` if it has a value.
std::vector<PerformanceEntry*> Merge(
    const std::vector<PerformanceEntry*>& first,
    const std::vector<PerformanceEntry*>& second,
    const std::optional<std::u16string>& name) {
  std::vector<PerformanceEntry*> merged;
  merged.reserve(first.size() + second.size());
  auto first_it = first.begin();
  auto second_it = second.begin();
  const auto skip = [&] {
    while (second_it != second.end() && !HasName(*second_it, name)) {
      ++second_it;
    }
  };
  skip();
  while (first_it != first.end()) {
    if (second_it == second.end()) {
      merged.insert(merged.end(), first_it, first.end());
      break;
    }
    if (PerformanceEntry::StartTimeCompareLessThan(*second_it, *first_it)) {
      merged.push_back(*second_it++);
      skip();
    } else {
      merged.push_back(*first_it++);
    }
  }
  while (second_it != second.end()) {
    merged.push_back(*second_it++);
    skip();
  }
  return merged;
}

// Performance::InsertEntryIntoSortedBuffer(): at the end, then moved back
// past those that start later.
void InsertEntryIntoSortedBuffer(HeapVector<PerformanceEntry>& entries,
                                 PerformanceEntry& entry) {
  size_t index = entries.size();
  while (index > 0 && entries[index - 1]->StartTime() > entry.StartTime()) {
    --index;
  }
  entries.insert(index, &entry);
}

std::vector<PerformanceEntry*> Named(
    const HeapVector<PerformanceEntry>& list,
    const std::optional<std::u16string>& name) {
  std::vector<PerformanceEntry*> entries;
  for (PerformanceEntry* const entry : list.ToVector()) {
    if (HasName(entry, name)) {
      entries.push_back(entry);
    }
  }
  return entries;
}

// UserTiming::ClearPerformanceEntries().
void ClearEntries(HeapVector<PerformanceEntry>& list,
                  const std::optional<std::u16string>& name) {
  if (!name) {
    list.clear();
    return;
  }
  list.EraseIf([&](PerformanceEntry* entry) { return entry->Name() == *name; });
}

std::string Utf8(const std::u16string& text) {
  return base::UTF16ToUTF8(text);
}

}  // namespace

// static
Performance* Performance::Create(base::TimeTicks time_origin,
                                 DocumentLoadTiming* timing) {
  return bindings::MakeGarbageCollected<Performance>(time_origin, timing);
}

Performance::Performance(base::TimeTicks time_origin,
                         DocumentLoadTiming* timing)
    : time_origin_(time_origin),
      unix_at_zero_monotonic_(UnixAtZeroMonotonic()),
      document_timing_(timing),
      navigation_id_(static_cast<uint64_t>(
          base::RandIntInclusive(kMinNavigationId, kMaxNavigationIdForReset))),
      resource_timing_buffer_size_limit_(kDefaultResourceTimingBufferSize) {
  // WindowPerformance's AddVisibilityStateEntry() for the page's visibility
  // when it is made, at no time, which reads as 0.
  visibility_state_buffer_.push_back(
      bindings::MakeGarbageCollected<VisibilityStateEntry>(u"visible", 0.0,
                                                           navigation_id_));
}

Performance::~Performance() = default;

const bindings::WrapperTypeInfo* Performance::GetWrapperTypeInfo() const {
  return &bindings::V8Performance::kWrapperTypeInfo;
}

void Performance::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(document_timing_);
  visitor->Trace(timing_);
  visitor->Trace(navigation_);
  visitor->Trace(navigation_timing_);
  resource_timing_buffer_.Trace(visitor);
  resource_timing_secondary_buffer_.Trace(visitor);
  marks_.Trace(visitor);
  measures_.Trace(visitor);
  visibility_state_buffer_.Trace(visitor);
  EventTarget::Trace(visitor);
}

double Performance::Now() const {
  return MonotonicTimeToDOMHighResTimeStamp(time_origin_,
                                            base::TimeTicks::Now());
}

double Performance::TimeOrigin() const {
  return ClampTimeResolution(
      (unix_at_zero_monotonic_ - base::Time::UnixEpoch()) +
      (time_origin_ - base::TimeTicks()));
}

PerformanceNavigationTiming* Performance::NavigationTiming() {
  // Blink makes it when the document commits, which a document of a response
  // does before any of its scripts runs.
  if (!navigation_timing_ && document_timing_->has_response) {
    navigation_timing_ =
        bindings::MakeGarbageCollected<PerformanceNavigationTiming>(
            document_timing_.Get(), time_origin_, navigation_id_);
  }
  return navigation_timing_.Get();
}

Performance::EntryList Performance::EntriesOfFrame(
    const std::optional<std::u16string>& name) {
  EntryList entries =
      Merge(EntryList(), resource_timing_buffer_.ToVector(), name);
  if (PerformanceNavigationTiming* const navigation = NavigationTiming();
      navigation && HasName(navigation, name)) {
    // InsertEntryIntoSortedBuffer() on a plain list.
    auto at = entries.end();
    while (at != entries.begin() &&
           (*(at - 1))->StartTime() > navigation->StartTime()) {
      --at;
    }
    entries.insert(at, navigation);
  }
  entries = Merge(entries, visibility_state_buffer_.ToVector(), name);
  entries = Merge(entries, Named(marks_, name), std::nullopt);
  entries = Merge(entries, Named(measures_, name), std::nullopt);
  return entries;
}

Performance::EntryList Performance::EntriesOfType(
    const std::u16string& entry_type,
    const std::optional<std::u16string>& name) {
  const PerformanceEntry::Kind kind = PerformanceEntry::ToKind(entry_type);
  if (!PerformanceEntry::IsValidTimelineEntryKind(kind)) {
    Window::Current().AddConsoleMessage(ConsoleLevel::kWarning,
                                        "Deprecated API for given entry type.");
    return EntryList();
  }
  EntryList entries;
  switch (kind) {
    case PerformanceEntry::kResource:
      entries = resource_timing_buffer_.ToVector();
      break;
    case PerformanceEntry::kNavigation:
      if (PerformanceNavigationTiming* const navigation = NavigationTiming()) {
        entries.push_back(navigation);
      }
      break;
    case PerformanceEntry::kMark:
      return Named(marks_, name);
    case PerformanceEntry::kMeasure:
      return Named(measures_, name);
    case PerformanceEntry::kVisibilityState:
      entries = visibility_state_buffer_.ToVector();
      break;
    default:
      // Paint, first input, the back/forward cache, soft navigations and long
      // animation frames never come to a Page.
      break;
  }
  if (!name) {
    return entries;
  }
  EntryList named;
  for (PerformanceEntry* const entry : entries) {
    if (entry->Name() == *name) {
      named.push_back(entry);
    }
  }
  return named;
}

std::vector<PerformanceEntry*> Performance::GetEntries() {
  return EntriesOfFrame(std::nullopt);
}

std::vector<PerformanceEntry*> Performance::GetEntriesByType(
    const std::u16string& entry_type) {
  return EntriesOfType(entry_type, std::nullopt);
}

std::vector<PerformanceEntry*> Performance::GetEntriesByName(
    const std::u16string& name,
    const std::optional<std::u16string>& entry_type) {
  if (!entry_type) {
    return EntriesOfFrame(name);
  }
  return EntriesOfType(*entry_type, name);
}

void Performance::ClearResourceTimings() {
  resource_timing_buffer_.clear();
}

void Performance::SetResourceTimingBufferSize(uint32_t max_size) {
  resource_timing_buffer_size_limit_ = max_size;
}

PerformanceTiming* Performance::Timing() {
  if (!timing_) {
    timing_ = bindings::MakeGarbageCollected<PerformanceTiming>(
        document_timing_.Get());
  }
  return timing_.Get();
}

PerformanceNavigation* Performance::Navigation() {
  if (!navigation_) {
    navigation_ = bindings::MakeGarbageCollected<PerformanceNavigation>(
        document_timing_.Get());
  }
  return navigation_.Get();
}

PerformanceMark* Performance::Mark(
    const std::u16string& mark_name,
    const bindings::PerformanceMarkOptions& mark_options,
    bindings::ExceptionState& exception_state) {
  PerformanceMark* const mark =
      PerformanceMark::Create(mark_name, mark_options, exception_state);
  if (mark) {
    InsertEntryIntoSortedBuffer(marks_, *mark);
  }
  return mark;
}

void Performance::ClearMarks(const std::optional<std::u16string>& mark_name) {
  ClearEntries(marks_, mark_name);
}

PerformanceMeasure* Performance::Measure(
    const std::u16string& measure_name,
    const bindings::DOMStringOrPerformanceMeasureOptions&
        start_or_measure_options,
    const std::optional<std::u16string>& end_mark,
    bindings::ExceptionState& exception_state) {
  // Performance::MeasureInternal().
  if (const auto* const options =
          std::get_if<bindings::PerformanceMeasureOptions>(
              &start_or_measure_options)) {
    const bool empty = !options->detail && !options->end && !options->start &&
                       !options->duration;
    if (!empty) {
      if (end_mark) {
        exception_state.ThrowTypeError(
            "If a non-empty PerformanceMeasureOptions object was passed, "
            "|end_mark| must not be passed.");
        return nullptr;
      }
      if (!options->start && !options->end) {
        exception_state.ThrowTypeError(
            "If a non-empty PerformanceMeasureOptions object was passed, at "
            "least one of its 'start' or 'end' properties must be present.");
        return nullptr;
      }
      if (options->start && options->duration && options->end) {
        exception_state.ThrowTypeError(
            "If a non-empty PerformanceMeasureOptions object was passed, it "
            "must not have all of its 'start', 'duration', and 'end' "
            "properties defined");
        return nullptr;
      }
      const auto to_mark_or_time =
          [](const std::optional<bindings::DOMStringOrDouble>& value)
          -> std::optional<MarkOrTime> {
        if (!value) {
          return std::nullopt;
        }
        if (const double* const time = std::get_if<double>(&*value)) {
          return MarkOrTime{.time = *time};
        }
        return MarkOrTime{.mark = std::get<std::u16string>(*value)};
      };
      return MeasureWithDetail(measure_name, to_mark_or_time(options->start),
                               options->duration, to_mark_or_time(options->end),
                               options->detail, exception_state);
    }
  }
  std::optional<MarkOrTime> start;
  if (const auto* const mark =
          std::get_if<std::u16string>(&start_or_measure_options)) {
    start = MarkOrTime{.mark = *mark};
  }
  std::optional<MarkOrTime> end;
  if (end_mark) {
    end = MarkOrTime{.mark = *end_mark};
  }
  return MeasureWithDetail(measure_name, start, std::nullopt, end, std::nullopt,
                           exception_state);
}

double Performance::GetTimeOrFindMarkTime(
    const std::u16string& measure_name,
    const MarkOrTime& mark_or_time,
    bindings::ExceptionState& exception_state) {
  if (!mark_or_time.mark) {
    if (mark_or_time.time < 0) {
      exception_state.ThrowTypeError(base::StrCat(
          {"'", Utf8(measure_name), "' cannot have a negative time stamp."}));
    }
    return mark_or_time.time;
  }
  // UserTiming::FindExistingMarkStartTime(): the latest mark of the name.
  const std::u16string& mark_name = *mark_or_time.mark;
  const std::vector<PerformanceEntry*> marks = Named(marks_, mark_name);
  if (!marks.empty()) {
    return marks.back()->StartTime();
  }
  // Or an attribute of performance.timing, relative to the navigation's
  // start.
  const std::optional<uint64_t> value = Timing()->GetNamedAttribute(mark_name);
  if (!value) {
    exception_state.ThrowDOMException(
        "SyntaxError",
        base::StrCat({"The mark '", Utf8(mark_name), "' does not exist."}));
    return 0;
  }
  if (*value == 0) {
    exception_state.ThrowDOMException(
        "InvalidAccessError",
        base::StrCat({"'", Utf8(mark_name),
                      "' is empty: either the event hasn't happened yet, or "
                      "it would provide cross-origin timing information."}));
    return 0;
  }
  return static_cast<double>(*value) -
         static_cast<double>(Timing()->NavigationStart());
}

PerformanceMeasure* Performance::MeasureWithDetail(
    const std::u16string& measure_name,
    const std::optional<MarkOrTime>& start,
    std::optional<double> duration,
    const std::optional<MarkOrTime>& end,
    std::optional<v8::Local<v8::Value>> detail,
    bindings::ExceptionState& exception_state) {
  double start_time =
      start ? GetTimeOrFindMarkTime(measure_name, *start, exception_state) : 0;
  if (exception_state.HadException()) {
    return nullptr;
  }
  double end_time =
      end ? GetTimeOrFindMarkTime(measure_name, *end, exception_state) : Now();
  if (exception_state.HadException()) {
    return nullptr;
  }
  if (duration) {
    // With a duration, exactly one of start and end was given.
    if (!start) {
      start_time = end_time - *duration;
    } else {
      end_time = start_time + *duration;
    }
  }
  // PerformanceMeasure::Create().
  std::optional<SerializedScriptValue> serialized;
  if (detail) {
    serialized =
        SerializedScriptValue::Serialize(v8::Isolate::GetCurrent(), *detail);
    if (!serialized) {
      return nullptr;
    }
  }
  PerformanceMeasure* const measure =
      bindings::MakeGarbageCollected<PerformanceMeasure>(
          measure_name, start_time, end_time, std::move(serialized),
          navigation_id_);
  InsertEntryIntoSortedBuffer(measures_, *measure);
  return measure;
}

void Performance::ClearMeasures(
    const std::optional<std::u16string>& measure_name) {
  ClearEntries(measures_, measure_name);
}

MemoryInfo* Performance::Memory() const {
  return MemoryInfo::Create(MemoryInfo::IsProcessLockedToSite()
                                ? MemoryInfo::Precision::kPrecise
                                : MemoryInfo::Precision::kBucketized);
}

v8::Local<v8::Object> Performance::ToJSONForBinding() {
  // WindowPerformance::BuildJSONValue(): the timing and the navigation as
  // their objects, which JSON.stringify() turns into JSON in turn.
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  V8ObjectBuilder builder(isolate);
  builder.AddNumber("timeOrigin", TimeOrigin());
  builder.Add("timing", bindings::ToV8(isolate, Timing()));
  builder.Add("navigation", bindings::ToV8(isolate, Navigation()));
  return builder.Result();
}

bool Performance::CanAddResourceTimingEntry() const {
  return resource_timing_buffer_.size() < resource_timing_buffer_size_limit_;
}

void Performance::AddResourceTiming(ResourceTimingInfo info,
                                    const std::u16string& initiator_type) {
  PerformanceResourceTiming* const entry =
      bindings::MakeGarbageCollected<PerformanceResourceTiming>(
          std::move(info), initiator_type, time_origin_, navigation_id_);
  // https://w3c.github.io/resource-timing/#dfn-add-a-performanceresourcetiming-entry
  if (CanAddResourceTimingEntry() &&
      !resource_timing_buffer_full_event_pending_) {
    InsertEntryIntoSortedBuffer(resource_timing_buffer_, *entry);
    return;
  }
  if (!resource_timing_buffer_full_event_pending_) {
    resource_timing_buffer_full_event_pending_ = true;
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    // WindowPerformance's timers run on the performance timeline task source.
    EventLoop::Get()
        ->TaskRunner(TaskType::kPerformanceTimeline)
        ->PostTask(
            FROM_HERE,
            base::BindOnce(
                [](v8::Isolate* isolate, v8::Global<v8::Context> context_handle,
                   cppgc::Persistent<Performance> performance) {
                  const v8::HandleScope handle_scope(isolate);
                  const v8::Local<v8::Context> context =
                      context_handle.Get(isolate);
                  const v8::Context::Scope context_scope(context);
                  // A window that has gone fires nothing.
                  const Window* const window = bindings::V8Window::ToWrappable(
                      isolate, context->Global());
                  if (!window || !window->HasContext()) {
                    return;
                  }
                  performance->FireResourceTimingBufferFull();
                },
                base::Unretained(isolate),
                v8::Global<v8::Context>(isolate, isolate->GetCurrentContext()),
                cppgc::Persistent<Performance>(this)));
  }
  resource_timing_secondary_buffer_.push_back(entry);
}

void Performance::FireResourceTimingBufferFull() {
  // https://w3c.github.io/resource-timing/#dfn-fire-a-buffer-full-event
  while (!resource_timing_secondary_buffer_.empty()) {
    const size_t excess_before = resource_timing_secondary_buffer_.size();
    if (!CanAddResourceTimingEntry()) {
      // Blink's EventTarget::DispatchEvent() makes the event trusted.
      bindings::EventInit init;
      Event* const event = Event::Create(u"resourcetimingbufferfull", init);
      event->SetTrusted(true);
      DispatchEvent(event);
    }
    // Copies the secondary buffer while there is room.
    while (!resource_timing_secondary_buffer_.empty() &&
           CanAddResourceTimingEntry()) {
      PerformanceEntry* const entry = resource_timing_secondary_buffer_[0];
      resource_timing_secondary_buffer_.EraseIf(
          [entry](PerformanceEntry* item) { return item == entry; });
      resource_timing_buffer_.push_back(entry);
    }
    if (resource_timing_secondary_buffer_.size() >= excess_before) {
      resource_timing_secondary_buffer_.clear();
      break;
    }
  }
  resource_timing_buffer_full_event_pending_ = false;
}

// static
double Performance::ClampTimeResolution(base::TimeDelta time) {
  // One secret for the process, as Blink keeps one static clamper.
  static const base::NoDestructor<TimeClamper> clamper;
  return clamper->ClampTimeResolution(time, kCrossOriginIsolated)
      .InMillisecondsF();
}

// static
double Performance::MonotonicTimeToDOMHighResTimeStamp(
    base::TimeTicks time_origin,
    base::TimeTicks time) {
  // No raw timestamp of the platform's goes out.
  if (time.is_null() || time_origin.is_null()) {
    return 0.0;
  }
  const double clamped_time = ClampTimeResolution(time.since_origin()) -
                              ClampTimeResolution(time_origin.since_origin());
  return clamped_time < 0 ? 0.0 : clamped_time;
}

}  // namespace mitto
