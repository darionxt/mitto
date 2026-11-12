// The Crash Reporting key-value API's CrashReportContext, the window's
// crashReport, which a page of a secure context has: the object alone. Its
// methods are Known Gaps.
//
// Upstream: third_party/blink/renderer/core/frame/crash_report_context.cc

#ifndef MITTO_DOM_CRASH_REPORT_CONTEXT_H_
#define MITTO_DOM_CRASH_REPORT_CONTEXT_H_

#include "mitto/bindings/script_wrappable.h"

namespace mitto {

class CrashReportContext final : public bindings::ScriptWrappable {
 public:
  static CrashReportContext* Create();
  // The one a Known Gap gives, whose wrapper is of `info`.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  CrashReportContext();
  ~CrashReportContext() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
};

}  // namespace mitto

#endif  // MITTO_DOM_CRASH_REPORT_CONTEXT_H_
