// The Presentation API's PresentationRequest, as Chrome answers a page whose
// browser finds no display to present to: the constructor's checks of the
// URLs, an availability that stays false, and no presentation to reconnect
// to. start(), which needs a user's activation, is a Known Gap.
//
// Upstream: third_party/blink/renderer/modules/presentation/presentation_request.cc
// Upstream: third_party/blink/renderer/modules/presentation/presentation_availability_state.cc
// Upstream: third_party/blink/renderer/modules/presentation/presentation_connection_callbacks.cc
// Upstream: third_party/blink/renderer/core/loader/mixed_content_checker.cc

#ifndef MITTO_DOM_PRESENTATION_REQUEST_H_
#define MITTO_DOM_PRESENTATION_REQUEST_H_

#include <string>
#include <vector>

#include "mitto/bindings/exception_state.h"
#include "mitto/dom/event_target.h"
#include "url/gurl.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-promise.h"

namespace mitto {

class PresentationAvailability;

class PresentationRequest final : public EventTarget {
 public:
  static PresentationRequest* Create(const std::u16string& url,
                                     bindings::ExceptionState& exception_state);
  static PresentationRequest* Create(const std::vector<std::u16string>& urls,
                                     bindings::ExceptionState& exception_state);

  explicit PresentationRequest(std::vector<GURL> urls);
  ~PresentationRequest() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  v8::Local<v8::Promise> Reconnect(const std::u16string& id,
                                   bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> GetAvailability(
      bindings::ExceptionState& exception_state);

 private:
  const std::vector<GURL> urls_;
  // The same object for each call, made by the first.
  cppgc::Member<PresentationAvailability> availability_;
  // Whether the browser has answered the first request of the availability,
  // after which a request settles at once.
  bool availability_known_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_PRESENTATION_REQUEST_H_
