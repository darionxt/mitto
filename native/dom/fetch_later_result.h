// The Fetch Standard's FetchLaterResult, what fetchLater() gives: whether its
// deferred request has gone.
//
// Upstream: third_party/blink/renderer/core/fetch/fetch_later_result.cc

#ifndef MITTO_DOM_FETCH_LATER_RESULT_H_
#define MITTO_DOM_FETCH_LATER_RESULT_H_

#include "mitto/bindings/script_wrappable.h"

namespace mitto {

class FetchLaterResult final : public bindings::ScriptWrappable {
 public:
  FetchLaterResult();
  ~FetchLaterResult() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  bool Activated() const { return activated_; }

  void SetActivated() { activated_ = true; }

 private:
  bool activated_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_FETCH_LATER_RESULT_H_
