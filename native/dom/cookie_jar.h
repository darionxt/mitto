// What a document's cookie attribute reads and writes: the cookies of the
// Session that loaded the document, for its URL. The Page program gives a
// document that its Session loaded one, as Blink gives a document of a frame
// its CookieJar, and the DOM knows nothing of how it reaches the Session.
//
// Upstream: third_party/blink/renderer/core/loader/cookie_jar.h

#ifndef MITTO_DOM_COOKIE_JAR_H_
#define MITTO_DOM_COOKIE_JAR_H_

#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "mitto/page_messages.h"

namespace mitto {

class CookieJar {
 public:
  // The document's cookies, as a Cookie header's line holds them, without
  // HttpOnly ones. Blocks until the Session answers, as Blink's does.
  virtual std::u16string Cookies() = 0;

  // Has the Session store a cookie as a page's script sets it, and returns at
  // once. A Cookies() after it sees it.
  virtual void SetCookie(const std::u16string& line) = 0;

  // What the document's CookieStore asks of the Session, as Blink's asks
  // RestrictedCookieManager: each answer comes from a task of its own. The
  // question's number is the jar's to give.
  using CookiesAnswer = base::OnceCallback<void(std::vector<PageCookie>)>;
  virtual void GetForCookieStore(PageCookieStoreGet get,
                                 CookiesAnswer answer) = 0;
  using StoredAnswer = base::OnceCallback<void(bool stored)>;
  virtual void SetForCookieStore(PageCookieStoreSet set,
                                 StoredAnswer answer) = 0;
  // Has the Session tell `listener` of each change to the document's cookies,
  // each from a task of its own, or stop, for a null one.
  using ChangeListener =
      base::RepeatingCallback<void(const PageCookieStoreChange&)>;
  virtual void ListenForCookieStore(ChangeListener listener) = 0;

 protected:
  virtual ~CookieJar() = default;
};

}  // namespace mitto

#endif  // MITTO_DOM_COOKIE_JAR_H_
