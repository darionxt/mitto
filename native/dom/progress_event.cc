#include "mitto/dom/progress_event.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_progress_event.h"

namespace mitto {

// static
ProgressEvent* ProgressEvent::Create(const std::u16string& type,
                                     const bindings::ProgressEventInit& init) {
  return bindings::MakeGarbageCollected<ProgressEvent>(type, init);
}

// static
ProgressEvent* ProgressEvent::CreateTrusted(const std::u16string& type,
                                            bool length_computable,
                                            uint64_t loaded,
                                            uint64_t total) {
  bindings::ProgressEventInit init;
  init.length_computable = length_computable;
  init.loaded = static_cast<double>(loaded);
  init.total = static_cast<double>(total);
  ProgressEvent* const event = Create(type, init);
  event->SetTrusted(true);
  return event;
}

ProgressEvent::ProgressEvent(const std::u16string& type,
                             const bindings::ProgressEventInit& init)
    : Event(type, init),
      length_computable_(init.length_computable),
      loaded_(init.loaded),
      total_(init.total) {}

ProgressEvent::~ProgressEvent() = default;

const bindings::WrapperTypeInfo* ProgressEvent::GetWrapperTypeInfo() const {
  return &bindings::V8ProgressEvent::kWrapperTypeInfo;
}

}  // namespace mitto
