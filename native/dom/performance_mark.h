// User Timing's PerformanceMark, which performance.mark() and its constructor
// make, with Blink's checks and its detail: a structured clone of what the
// page gave, read back once for each context.
//
// Upstream: third_party/blink/renderer/core/timing/performance_mark.h
// Upstream: third_party/blink/renderer/core/timing/performance_mark.cc

#ifndef MITTO_DOM_PERFORMANCE_MARK_H_
#define MITTO_DOM_PERFORMANCE_MARK_H_

#include <optional>
#include <string>

#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/dom/performance_entry.h"
#include "mitto/dom/serialized_script_value.h"
#include "v8/include/v8-traced-handle.h"

namespace mitto {

class PerformanceMark final : public PerformanceEntry {
 public:
  // new PerformanceMark(), and performance.mark(), for the window's
  // performance. Throws and returns null as Blink's Create() does.
  static PerformanceMark* Create(
      const std::u16string& mark_name,
      const bindings::PerformanceMarkOptions& options,
      bindings::ExceptionState& exception_state);

  PerformanceMark(std::u16string name,
                  double start_time,
                  std::optional<SerializedScriptValue> detail,
                  uint64_t navigation_id);
  ~PerformanceMark() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  v8::Local<v8::Value> Detail();

  // PerformanceEntry:
  Kind GetKind() const override;
  std::u16string_view TypeName() const override;

 private:
  const std::optional<SerializedScriptValue> detail_;
  // What Detail() read back, which it gives again.
  v8::TracedReference<v8::Value> deserialized_detail_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PERFORMANCE_MARK_H_
