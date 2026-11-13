#include "mitto/dom/performance_navigation.h"

#include "mitto/bindings/v8_performance_navigation.h"
#include "mitto/dom/document_load_timing.h"
#include "mitto/dom/performance_entry.h"
#include "v8/include/v8-isolate.h"

namespace mitto {
namespace {

// PerformanceNavigation's TYPE_NAVIGATE, for kWebNavigationTypeOther.
constexpr uint16_t kTypeNavigate = 0;

}  // namespace

PerformanceNavigation::PerformanceNavigation(DocumentLoadTiming* timing)
    : timing_(timing) {}

PerformanceNavigation::~PerformanceNavigation() = default;

const bindings::WrapperTypeInfo* PerformanceNavigation::GetWrapperTypeInfo()
    const {
  return &bindings::V8PerformanceNavigation::kWrapperTypeInfo;
}

void PerformanceNavigation::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(timing_);
  bindings::ScriptWrappable::Trace(visitor);
}

uint16_t PerformanceNavigation::Type() const {
  return kTypeNavigate;
}

uint16_t PerformanceNavigation::RedirectCount() const {
  if (timing_->has_cross_origin_redirect) {
    return 0;
  }
  return timing_->redirect_count;
}

v8::Local<v8::Object> PerformanceNavigation::ToJSONForBinding() const {
  V8ObjectBuilder builder(v8::Isolate::GetCurrent());
  builder.AddNumber("type", Type());
  builder.AddNumber("redirectCount", RedirectCount());
  return builder.Result();
}

}  // namespace mitto
