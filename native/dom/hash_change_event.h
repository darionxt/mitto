// The HTML Standard's HashChangeEvent, which a window fires once a navigation
// to a fragment of its document has changed its URL, with the URL before and
// after.
//
// Upstream: third_party/blink/renderer/core/events/hash_change_event.h

#ifndef MITTO_DOM_HASH_CHANGE_EVENT_H_
#define MITTO_DOM_HASH_CHANGE_EVENT_H_

#include <string>

#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/event.h"

namespace mitto {

class HashChangeEvent final : public Event {
 public:
  static HashChangeEvent* Create(const std::u16string& type,
                                 const bindings::HashChangeEventInit& init);
  // A trusted hashchange, which neither bubbles nor can be canceled, as Blink's
  // window makes it.
  static HashChangeEvent* CreateTrusted(const std::u16string& old_url,
                                        const std::u16string& new_url);

  HashChangeEvent(const std::u16string& type,
                  const bindings::HashChangeEventInit& init);
  ~HashChangeEvent() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  const std::u16string& OldURL() const { return old_url_; }
  const std::u16string& NewURL() const { return new_url_; }

 private:
  const std::u16string old_url_;
  const std::u16string new_url_;
};

}  // namespace mitto

#endif  // MITTO_DOM_HASH_CHANGE_EVENT_H_
