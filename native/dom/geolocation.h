// Geolocation, navigator.geolocation, as Chrome answers a page whose profile
// has granted nothing: on a secure page Chrome's prompt waits for a person
// and gives no position, while each request's timeout runs, as Blink starts it
// before it asks; elsewhere every request fails at once with Blink's message
// that only secure origins are allowed.
//
// Upstream: third_party/blink/renderer/core/geolocation/geolocation.cc
// Upstream: third_party/blink/renderer/core/geolocation/geo_notifier.cc
// Upstream: third_party/blink/renderer/platform/weborigin/security_origin.cc

#ifndef MITTO_DOM_GEOLOCATION_H_
#define MITTO_DOM_GEOLOCATION_H_

#include <stddef.h>
#include <stdint.h>

#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-local-handle.h"

namespace mitto {

class GeoNotifier;

class Geolocation final : public bindings::ScriptWrappable {
 public:
  static Geolocation* Create();

  Geolocation();
  ~Geolocation() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  void GetCurrentPositionForBindings(v8::Local<v8::Function> success_callback,
                                     v8::Local<v8::Function> error_callback,
                                     const bindings::PositionOptions& options);
  int32_t WatchPositionForBindings(v8::Local<v8::Function> success_callback,
                                   v8::Local<v8::Function> error_callback,
                                   const bindings::PositionOptions& options);
  void ClearWatch(int32_t watch_id);

  // How many requests wait on a timer that will end them: a timeout, or an
  // error reported from a task of its own. A request with no timeout ends only
  // once a person answers the prompt, so it counts for none.
  size_t TimerCount() const;

  // What a request's timer does once it fires.
  void Fire(GeoNotifier* notifier);

 private:
  void StartRequest(GeoNotifier* notifier);
  void Remove(GeoNotifier* notifier);

  // The requests, one-shots and watches alike, in a list on the heap, as
  // EventTarget keeps its listeners.
  cppgc::Member<GeoNotifier> first_;
  int32_t last_watch_id_ = 0;
};

}  // namespace mitto

#endif  // MITTO_DOM_GEOLOCATION_H_
