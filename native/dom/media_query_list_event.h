// CSSOM View's MediaQueryListEvent, which a MediaQueryList's change event is.
// A Page's lists fire none, since its Machine does not change, so only a
// page's own events have one.
//
// Upstream: third_party/blink/renderer/core/css/media_query_list_event.h

#ifndef MITTO_DOM_MEDIA_QUERY_LIST_EVENT_H_
#define MITTO_DOM_MEDIA_QUERY_LIST_EVENT_H_

#include <string>

#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/event.h"

namespace mitto {

class MediaQueryListEvent final : public Event {
 public:
  static MediaQueryListEvent* Create(
      const std::u16string& type,
      const bindings::MediaQueryListEventInit& init);

  MediaQueryListEvent(const std::u16string& type,
                      const bindings::MediaQueryListEventInit& init);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  const std::u16string& Media() const { return media_; }
  bool Matches() const { return matches_; }

 private:
  const std::u16string media_;
  const bool matches_;
};

}  // namespace mitto

#endif  // MITTO_DOM_MEDIA_QUERY_LIST_EVENT_H_
