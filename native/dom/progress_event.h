// The XMLHttpRequest Standard's ProgressEvent, which FileReader and, with
// #283, XMLHttpRequest fire: how many bytes have come of how many, if that is
// known.
//
// Upstream: third_party/blink/renderer/core/events/progress_event.h
// Upstream: third_party/blink/renderer/core/events/progress_event.cc

#ifndef MITTO_DOM_PROGRESS_EVENT_H_
#define MITTO_DOM_PROGRESS_EVENT_H_

#include <stdint.h>

#include <string>

#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/event.h"

namespace mitto {

class ProgressEvent final : public Event {
 public:
  static ProgressEvent* Create(const std::u16string& type,
                               const bindings::ProgressEventInit& init);
  // A trusted one that neither bubbles nor can be canceled, as Blink's
  // ProgressEvent::Create(type, length_computable, loaded, total) makes it.
  static ProgressEvent* CreateTrusted(const std::u16string& type,
                                      bool length_computable,
                                      uint64_t loaded,
                                      uint64_t total);

  ProgressEvent(const std::u16string& type,
                const bindings::ProgressEventInit& init);
  ~ProgressEvent() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  bool LengthComputable() const { return length_computable_; }
  double Loaded() const { return loaded_; }
  double Total() const { return total_; }

 private:
  const bool length_computable_;
  const double loaded_;
  const double total_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PROGRESS_EVENT_H_
