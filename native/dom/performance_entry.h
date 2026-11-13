// Performance Timeline's PerformanceEntry: what each entry of the timeline
// has, as Blink's class gives it. Its descendants without a class, such as
// PerformancePaintTiming, are Known Gaps whose objects have an entry with
// nothing behind them, as a Page paints nothing and takes no input.
//
// Upstream: third_party/blink/renderer/core/timing/performance_entry.h
// Upstream: third_party/blink/renderer/core/timing/performance_entry.cc

#ifndef MITTO_DOM_PERFORMANCE_ENTRY_H_
#define MITTO_DOM_PERFORMANCE_ENTRY_H_

#include <stdint.h>

#include <string>
#include <string_view>

#include "base/memory/raw_ptr.h"
#include "mitto/bindings/script_wrappable.h"
#include "v8/include/v8-forward.h"

namespace mitto {

class V8ObjectBuilder;

class PerformanceEntry : public bindings::ScriptWrappable {
 public:
  // Blink's PerformanceEntry::EntryType, a bit for each kind of entry.
  enum Kind : uint32_t {
    kInvalid = 0,
    kNavigation = 1 << 0,
    kMark = 1 << 1,
    kMeasure = 1 << 2,
    kResource = 1 << 3,
    kLongTask = 1 << 4,
    kTaskAttribution = 1 << 5,
    kPaint = 1 << 6,
    kEvent = 1 << 7,
    kFirstInput = 1 << 8,
    kElement = 1 << 9,
    kLayoutShift = 1 << 10,
    kLargestContentfulPaint = 1 << 11,
    kVisibilityState = 1 << 12,
    kBackForwardCacheRestoration = 1 << 13,
    kSoftNavigation = 1 << 14,
    kLongAnimationFrame = 1 << 15,
    kScript = 1 << 16,
    kContainer = 1 << 17,
    kInteractionContentfulPaint = 1 << 18,
    kScroll = 1 << 19,
  };

  // An entry with nothing behind it, for a Known Gap that gives one of an
  // interface without a class that descends from PerformanceEntry.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  PerformanceEntry(std::u16string name,
                   double start_time,
                   double finish_time,
                   uint64_t navigation_id);
  ~PerformanceEntry() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  const std::u16string& Name() const { return name_; }
  std::u16string EntryType() const { return std::u16string(TypeName()); }
  double StartTime() const { return start_time_; }
  // PerformanceNavigationTiming's ends with the load event, which it does not
  // know when it is made.
  virtual double Duration() const;
  uint64_t NavigationId() const { return navigation_id_; }
  v8::Local<v8::Object> ToJSONForBinding() const;

  // What the Performance Timeline uses.
  virtual Kind GetKind() const;
  // The name of the entry's type, as a page reads it.
  virtual std::u16string_view TypeName() const;
  static Kind ToKind(std::u16string_view entry_type);
  // Whether getEntries() and getEntriesByType() give entries of `type`. Blink
  // counts an unknown type as one, of which there are none.
  static bool IsValidTimelineEntryKind(Kind kind);
  // The order of the timeline: by start time, the navigation's first, then in
  // the order they were made.
  static bool StartTimeCompareLessThan(const PerformanceEntry* a,
                                       const PerformanceEntry* b);

 protected:
  // Adds what toJSON() gives, as BuildJSONValue() does.
  virtual void BuildJSONValue(V8ObjectBuilder& builder) const;

 private:
  const double duration_;
  const std::u16string name_;
  const double start_time_;
  // The order in which the entries of the process were made.
  const uint64_t index_;
  const uint64_t navigation_id_;
};

// Blink's V8ObjectBuilder: an object that toJSON() gives, with data
// properties in the order they are added.
class V8ObjectBuilder {
 public:
  explicit V8ObjectBuilder(v8::Isolate* isolate);
  ~V8ObjectBuilder();

  V8ObjectBuilder(const V8ObjectBuilder&) = delete;
  V8ObjectBuilder& operator=(const V8ObjectBuilder&) = delete;

  v8::Isolate* isolate() const { return isolate_.get(); }
  void AddNumber(std::string_view name, double value);
  void AddString(std::string_view name, std::u16string_view value);
  void AddNull(std::string_view name);
  void Add(std::string_view name, v8::Local<v8::Value> value);
  v8::Local<v8::Object> Result() const;

 private:
  const raw_ptr<v8::Isolate> isolate_;
  const v8::Local<v8::Object> object_;
  bool failed_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_PERFORMANCE_ENTRY_H_
