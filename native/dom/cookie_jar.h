// What a document's cookie attribute reads and writes: the cookies of the
// Session that loaded the document, for its URL. The Page program gives a
// document that its Session loaded one, as Blink gives a document of a frame
// its CookieJar, and the DOM knows nothing of how it reaches the Session.
//
// Upstream: third_party/blink/renderer/core/loader/cookie_jar.h

#ifndef MITTO_DOM_COOKIE_JAR_H_
#define MITTO_DOM_COOKIE_JAR_H_

#include <string>

namespace mitto {

class CookieJar {
 public:
  // The document's cookies, as a Cookie header's line holds them, without
  // HttpOnly ones. Blocks until the Session answers, as Blink's does.
  virtual std::u16string Cookies() = 0;

  // Has the Session store a cookie as a page's script sets it, and returns at
  // once. A Cookies() after it sees it.
  virtual void SetCookie(const std::u16string& line) = 0;

 protected:
  virtual ~CookieJar() = default;
};

}  // namespace mitto

#endif  // MITTO_DOM_COOKIE_JAR_H_
