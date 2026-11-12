// The HTML Standard's History, the window's history: the session history of a
// tab that a Session opened, whose one entry is the document, which a page
// reads as Chrome gives it for a new tab's first document. Moving through the
// history and its state's changes, pushState() and the rest, are Known Gaps,
// and so is scrollRestoration, which keeps what a page sets.
//
// Upstream: third_party/blink/renderer/core/frame/history.cc

#ifndef MITTO_DOM_HISTORY_H_
#define MITTO_DOM_HISTORY_H_

#include <stdint.h>

#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-value.h"

namespace mitto {

class History final : public bindings::ScriptWrappable {
 public:
  static History* Create();
  // The one a Known Gap gives, whose wrapper is of `info`.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  History();
  ~History() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface. A tab's first document is its one entry,
  // with no state, whose null Blink gives as null.
  uint32_t Length(bindings::ExceptionState& exception_state) const { return 1; }
  v8::Local<v8::Value> State(bindings::ExceptionState& exception_state) const;
};

}  // namespace mitto

#endif  // MITTO_DOM_HISTORY_H_
