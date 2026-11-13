// The High Resolution Time standard's Performance, a window's `performance`,
// with now() and timeOrigin, coarsened as Chrome coarsens them, Blink's
// memory, and the Performance Timeline as Blink keeps it for a window: the
// navigation's entry, an entry for each of the Page's requests, User Timing's
// marks and measures, and the legacy timing and navigation. Entries of paint,
// input, layout and long tasks never come, since a Page paints nothing and
// takes no input.
//
// Upstream: third_party/blink/renderer/core/timing/performance.h
// Upstream: third_party/blink/renderer/core/timing/performance.cc
// Upstream: third_party/blink/renderer/core/timing/window_performance.cc
// Upstream: third_party/blink/renderer/core/timing/performance_user_timing.cc
// Upstream: third_party/blink/renderer/core/timing/performance_timeline_entry_id_generator.cc

#ifndef MITTO_DOM_PERFORMANCE_H_
#define MITTO_DOM_PERFORMANCE_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "base/time/time.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/dom/document_load_timing.h"
#include "mitto/dom/event_target.h"
#include "mitto/dom/heap_vector.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

class MemoryInfo;
class PerformanceEntry;
class PerformanceMark;
class PerformanceMeasure;
class PerformanceNavigation;
class PerformanceNavigationTiming;
class PerformanceTiming;

class Performance final : public EventTarget {
 public:
  // Counts from `time_origin`: the start of the navigation that loaded the
  // window's document, or when the window was made. `timing` is the window's,
  // which its document's loading fills.
  static Performance* Create(base::TimeTicks time_origin,
                             DocumentLoadTiming* timing);

  Performance(base::TimeTicks time_origin, DocumentLoadTiming* timing);
  ~Performance() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  double Now() const;
  double TimeOrigin() const;
  std::vector<PerformanceEntry*> GetEntries();
  std::vector<PerformanceEntry*> GetEntriesByType(
      const std::u16string& entry_type);
  std::vector<PerformanceEntry*> GetEntriesByName(
      const std::u16string& name,
      const std::optional<std::u16string>& entry_type);
  void ClearResourceTimings();
  void SetResourceTimingBufferSize(uint32_t max_size);
  PerformanceTiming* Timing();
  PerformanceNavigation* Navigation();
  PerformanceMark* Mark(const std::u16string& mark_name,
                        const bindings::PerformanceMarkOptions& mark_options,
                        bindings::ExceptionState& exception_state);
  void ClearMarks(const std::optional<std::u16string>& mark_name);
  PerformanceMeasure* Measure(
      const std::u16string& measure_name,
      const bindings::DOMStringOrPerformanceMeasureOptions&
          start_or_measure_options,
      const std::optional<std::u16string>& end_mark,
      bindings::ExceptionState& exception_state);
  void ClearMeasures(const std::optional<std::u16string>& measure_name);
  // WindowPerformance::memory(): a new MemoryInfo for each call, precise while
  // the process is locked to a site.
  MemoryInfo* Memory() const;
  v8::Local<v8::Object> ToJSONForBinding();

  // What the Page uses.
  //
  // The window's navigation ID, which Blink draws from 100 to 10000 for each
  // window and counts up by 7 for each soft navigation, of which a Page has
  // none.
  uint64_t NavigationId() const { return navigation_id_; }
  // Adds the entry of a request of the window's, as Blink's
  // AddResourceTiming() does once the request has ended.
  void AddResourceTiming(ResourceTimingInfo info,
                         const std::u16string& initiator_type);

  // Blink's Performance::ClampTimeResolution(): `time` in milliseconds,
  // coarsened as every time that a page reads is, with the process's one
  // secret, so that two readings of one moment agree.
  static double ClampTimeResolution(base::TimeDelta time);

  // Blink's MonotonicTimeToDOMHighResTimeStamp(): `time` in milliseconds since
  // `time_origin`, each coarsened before the difference, and 0 for a time
  // before it.
  static double MonotonicTimeToDOMHighResTimeStamp(base::TimeTicks time_origin,
                                                   base::TimeTicks time);

 private:
  using EntryList = std::vector<PerformanceEntry*>;
  // A time or the name of a mark, as User Timing takes the ends of a measure.
  struct MarkOrTime {
    std::optional<std::u16string> mark;
    double time = 0;
  };

  // GetEntriesForCurrentFrame() and GetEntriesByTypeForCurrentFrame(), of
  // the entries named `name` if it has a value.
  EntryList EntriesOfFrame(const std::optional<std::u16string>& name);
  EntryList EntriesOfType(const std::u16string& entry_type,
                          const std::optional<std::u16string>& name);
  PerformanceNavigationTiming* NavigationTiming();
  // UserTiming::Measure(), with the ends that MeasureInternal() worked out.
  PerformanceMeasure* MeasureWithDetail(
      const std::u16string& measure_name,
      const std::optional<MarkOrTime>& start,
      std::optional<double> duration,
      const std::optional<MarkOrTime>& end,
      std::optional<v8::Local<v8::Value>> detail,
      bindings::ExceptionState& exception_state);
  // UserTiming::GetTimeOrFindMarkTime().
  double GetTimeOrFindMarkTime(const std::u16string& measure_name,
                               const MarkOrTime& mark_or_time,
                               bindings::ExceptionState& exception_state);
  bool CanAddResourceTimingEntry() const;
  // The HTML Standard's steps that fire resourcetimingbufferfull, from a
  // task of their own, while entries wait in the secondary buffer.
  void FireResourceTimingBufferFull();

  const base::TimeTicks time_origin_;
  // The Unix time at the zero of base::TimeTicks, from which timeOrigin is
  // given as a Unix time.
  const base::Time unix_at_zero_monotonic_;
  const cppgc::Member<DocumentLoadTiming> document_timing_;
  const uint64_t navigation_id_;
  cppgc::Member<PerformanceTiming> timing_;
  cppgc::Member<PerformanceNavigation> navigation_;
  cppgc::Member<PerformanceNavigationTiming> navigation_timing_;
  HeapVector<PerformanceEntry> resource_timing_buffer_;
  HeapVector<PerformanceEntry> resource_timing_secondary_buffer_;
  uint32_t resource_timing_buffer_size_limit_;
  bool resource_timing_buffer_full_event_pending_ = false;
  // UserTiming's buffers, each in the timeline's order.
  HeapVector<PerformanceEntry> marks_;
  HeapVector<PerformanceEntry> measures_;
  // The page's visibility, which only the first entry gives a Page.
  HeapVector<PerformanceEntry> visibility_state_buffer_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PERFORMANCE_H_
