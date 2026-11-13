// Navigation Timing's legacy PerformanceNavigation, performance.navigation:
// the navigation's type, which for a Page is always a navigation, as a typed
// URL is, and how many redirects it followed, none if one went to another
// origin.
//
// Upstream: third_party/blink/renderer/core/timing/performance_navigation.h
// Upstream: third_party/blink/renderer/core/timing/performance_navigation.cc

#ifndef MITTO_DOM_PERFORMANCE_NAVIGATION_H_
#define MITTO_DOM_PERFORMANCE_NAVIGATION_H_

#include <stdint.h>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/v8-forward.h"

namespace mitto {

class DocumentLoadTiming;

class PerformanceNavigation final : public bindings::ScriptWrappable {
 public:
  explicit PerformanceNavigation(DocumentLoadTiming* timing);
  ~PerformanceNavigation() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  uint16_t Type() const;
  uint16_t RedirectCount() const;
  v8::Local<v8::Object> ToJSONForBinding() const;

 private:
  const cppgc::Member<DocumentLoadTiming> timing_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PERFORMANCE_NAVIGATION_H_
