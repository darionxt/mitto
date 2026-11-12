#include "mitto/dom/crash_report_context.h"

#include "mitto/bindings/v8_crash_report_context.h"

namespace mitto {

// static
CrashReportContext* CrashReportContext::Create() {
  return bindings::MakeGarbageCollected<CrashReportContext>();
}

// static
bindings::ScriptWrappable* CrashReportContext::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<CrashReportContext>(
      info, bindings::V8CrashReportContext::kWrapperTypeInfo);
}

CrashReportContext::CrashReportContext() = default;
CrashReportContext::~CrashReportContext() = default;

const bindings::WrapperTypeInfo* CrashReportContext::GetWrapperTypeInfo()
    const {
  return &bindings::V8CrashReportContext::kWrapperTypeInfo;
}

}  // namespace mitto
