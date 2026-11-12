// The Network Information API's NetworkInformation, what navigator.connection
// gives: the Session's network estimate, with the noise that the renderer
// process of the document's site adds for its host, as a fetch() from the
// document to its own host sends it in rtt and downlink. The library works the
// values out, from the code that its requests' hints come from. Its event
// handler attributes are Known Gaps, and the estimate never changes, so no
// change event comes.
//
// Upstream: third_party/blink/renderer/modules/netinfo/network_information.cc
// Upstream: third_party/blink/renderer/platform/network/network_state_notifier.cc

#ifndef MITTO_DOM_NETWORK_INFORMATION_H_
#define MITTO_DOM_NETWORK_INFORMATION_H_

#include <stdint.h>

#include <string>

#include "mitto/dom/event_target.h"
#include "mitto/page_messages.h"

namespace mitto {

class NetworkInformation final : public EventTarget {
 public:
  // `effective_type` is the Machine's, such as "4g".
  static NetworkInformation* Create(const PageConnection& connection,
                                    const std::string& effective_type);

  NetworkInformation(const PageConnection& connection,
                     const std::string& effective_type);
  ~NetworkInformation() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  std::u16string EffectiveType() const;
  uint64_t Rtt() const;
  double Downlink() const;
  bool SaveData() const;

 private:
  const PageConnection connection_;
  const std::u16string effective_type_;
};

}  // namespace mitto

#endif  // MITTO_DOM_NETWORK_INFORMATION_H_
