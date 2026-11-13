// window.cookieStore: the Cookie Store API of a window, as Blink's CookieStore
// runs it over the Session's cookies, which the document's CookieJar reaches.
// get() and getAll() read the document's cookies that a script may, and set()
// and delete() store secure cookies, with Blink's checks of their names,
// domains and paths before anything goes to the Session. Each promise settles
// from a task of its own, once the Session has answered. While a page listens
// for change events, the Session tells the store of each change to the
// document's cookies, and the store fires one for it.
//
// Upstream: third_party/blink/renderer/modules/cookie_store/cookie_store.cc
// Upstream: net/cookies/cookie_util.cc

#ifndef MITTO_DOM_COOKIE_STORE_H_
#define MITTO_DOM_COOKIE_STORE_H_

#include <optional>
#include <string>
#include <string_view>

#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/event_target.h"
#include "mitto/page_messages.h"
#include "url/gurl.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-promise.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class CookieJar;
class Window;

class CookieStore final : public EventTarget {
 public:
  explicit CookieStore(Window* window);
  ~CookieStore() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  v8::Local<v8::Promise> Get(const std::u16string& name,
                             bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Get(const bindings::CookieStoreGetOptions& options,
                             bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> GetAll(const std::u16string& name,
                                bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> GetAll(const bindings::CookieStoreGetOptions& options,
                                bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Set(const std::u16string& name,
                             const std::u16string& value,
                             bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Set(const bindings::CookieInit& options,
                             bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Delete(const std::u16string& name,
                                bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Delete(
      const bindings::CookieStoreDeleteOptions& options,
      bindings::ExceptionState& exception_state);

  // Whether `domain`, lowercase and with a leading dot, may be the domain of
  // a cookie that a document at `url` sets, as net's
  // GetCookieDomainWithString() decides.
  static bool IsCookieDomainValid(const GURL& url, std::string_view domain);

 protected:
  void ListenersChanged(std::u16string_view type) override;

 private:
  // The document's jar, if its origin may reach cookies, or null after
  // throwing a SecurityError.
  CookieJar* JarForAccess(bindings::ExceptionState& exception_state) const;
  v8::Local<v8::Promise> DoRead(const bindings::CookieStoreGetOptions& options,
                                bool get_all,
                                bindings::ExceptionState& exception_state);
  // `method` is the member that writes, whose name a rejection says, as the
  // context of Blink's resolver does.
  v8::Local<v8::Promise> DoWrite(std::string_view method,
                                 const bindings::CookieInit& options,
                                 bindings::ExceptionState& exception_state);
  // ToCookieParams(): the cookie that `options` describe, or nothing after
  // throwing why not.
  std::optional<PageCookieStoreSet> ToCookieParams(
      const bindings::CookieInit& options,
      bindings::ExceptionState& exception_state) const;
  void StartObserving();
  void StopObserving();
  void OnCookieChange(const PageCookieStoreChange& change);

  const cppgc::WeakMember<Window> window_;
  bool observing_ = false;
  // Whether the store stops observing when the context goes.
  bool stops_with_context_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_COOKIE_STORE_H_
