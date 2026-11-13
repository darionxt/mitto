// Navigation Timing's legacy PerformanceTiming, performance.timing: the times
// of the window's navigation and document in whole milliseconds since the Unix
// epoch, from its DocumentLoadTiming, as Blink's class gives them.
//
// Upstream: third_party/blink/renderer/core/timing/performance_timing.h
// Upstream: third_party/blink/renderer/core/timing/performance_timing.cc

#ifndef MITTO_DOM_PERFORMANCE_TIMING_H_
#define MITTO_DOM_PERFORMANCE_TIMING_H_

#include <stdint.h>

#include <optional>
#include <string_view>

#include "base/time/time.h"
#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/v8-forward.h"

namespace mitto {

class DocumentLoadTiming;

class PerformanceTiming final : public bindings::ScriptWrappable {
 public:
  explicit PerformanceTiming(DocumentLoadTiming* timing);
  ~PerformanceTiming() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  uint64_t NavigationStart() const;
  uint64_t UnloadEventStart() const;
  uint64_t UnloadEventEnd() const;
  uint64_t RedirectStart() const;
  uint64_t RedirectEnd() const;
  uint64_t FetchStart() const;
  uint64_t DomainLookupStart() const;
  uint64_t DomainLookupEnd() const;
  uint64_t ConnectStart() const;
  uint64_t ConnectEnd() const;
  uint64_t SecureConnectionStart() const;
  uint64_t RequestStart() const;
  uint64_t ResponseStart() const;
  uint64_t ResponseEnd() const;
  uint64_t DomLoading() const;
  uint64_t DomInteractive() const;
  uint64_t DomContentLoadedEventStart() const;
  uint64_t DomContentLoadedEventEnd() const;
  uint64_t DomComplete() const;
  uint64_t LoadEventStart() const;
  uint64_t LoadEventEnd() const;
  v8::Local<v8::Object> ToJSONForBinding() const;

  // Whether `name` is an attribute of the interface, which a mark may not be
  // named and a measure may name.
  static bool IsAttributeName(std::u16string_view name);
  // The value of the attribute `name`, if it is one.
  std::optional<uint64_t> GetNamedAttribute(std::u16string_view name) const;

 private:
  uint64_t ToIntegerMilliseconds(base::TimeTicks time) const;

  const cppgc::Member<DocumentLoadTiming> timing_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PERFORMANCE_TIMING_H_
