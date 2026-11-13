// The Cookie Store API's CookieChangeEvent: the cookies that a change set and
// those that went, each a CookieListItem, as frozen arrays that are the same
// object each time. The CookieStore fires one for each change it hears of from
// the Session, and a page may make one of its own from an init dictionary.
//
// Upstream: third_party/blink/renderer/modules/cookie_store/cookie_change_event.h
// Upstream: third_party/blink/renderer/modules/cookie_store/cookie_change_event.cc

#ifndef MITTO_DOM_COOKIE_CHANGE_EVENT_H_
#define MITTO_DOM_COOKIE_CHANGE_EVENT_H_

#include <string>
#include <vector>

#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/event.h"
#include "mitto/page_messages.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-traced-handle.h"

namespace mitto {

class CookieChangeEvent final : public Event {
 public:
  static CookieChangeEvent* Create(const std::u16string& type,
                                   const bindings::CookieChangeEventInit& init);

  // A page's own event.
  CookieChangeEvent(const std::u16string& type,
                    const bindings::CookieChangeEventInit& init);
  // A change event of a CookieStore, which neither bubbles nor can be
  // canceled.
  CookieChangeEvent(std::vector<bindings::CookieListItem> changed,
                    std::vector<bindings::CookieListItem> deleted);
  ~CookieChangeEvent() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  v8::Local<v8::Value> Changed();
  v8::Local<v8::Value> Deleted();

  // CookieChangeEvent::ToCookieListItem(): a cookie that the Session gave,
  // without its value and expiry if it went.
  static bindings::CookieListItem ToCookieListItem(const PageCookie& cookie,
                                                   bool is_deleted);
  // The object of a CookieListItem's members, as Blink's generated code
  // writes a dictionary: only those present, in the order of their names.
  static v8::Local<v8::Object> ToV8Object(v8::Isolate* isolate,
                                          const bindings::CookieListItem& item);

 private:
  // The frozen array of `items`, made the first time into `array`.
  static v8::Local<v8::Value> SameFrozenArray(
      const std::vector<bindings::CookieListItem>& items,
      v8::TracedReference<v8::Array>& array);

  const std::vector<bindings::CookieListItem> changed_;
  const std::vector<bindings::CookieListItem> deleted_;
  v8::TracedReference<v8::Array> changed_array_;
  v8::TracedReference<v8::Array> deleted_array_;
};

}  // namespace mitto

#endif  // MITTO_DOM_COOKIE_CHANGE_EVENT_H_
