// document.cookie through the Session. A read asks over the channel and waits
// on the main thread for the answer, which the I/O thread hands over, as
// Blink's CookieJar waits for the browser in a synchronous call. A write goes
// without an answer, and the channel keeps the order, so a read after it, and
// a request after it, see the cookie.
//
// Upstream: third_party/blink/renderer/core/loader/cookie_jar.cc

#ifndef MITTO_PAGE_SESSION_COOKIE_JAR_H_
#define MITTO_PAGE_SESSION_COOKIE_JAR_H_

#include <stdint.h>

#include <optional>
#include <string>

#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "mitto/dom/cookie_jar.h"
#include "mitto/page/page_reporter.h"

namespace mitto {

class SessionCookieJar final : public CookieJar {
 public:
  SessionCookieJar();

  SessionCookieJar(const SessionCookieJar&) = delete;
  SessionCookieJar& operator=(const SessionCookieJar&) = delete;

  ~SessionCookieJar() override;

  // On the main thread, before the first read or write: how frames go to the
  // Session.
  void SetSend(SendFrame send);

  // CookieJar, on the main thread:
  std::u16string Cookies() override;
  void SetCookie(const std::u16string& line) override;

  // On the I/O thread: the Session's answer to question `number`. Returns
  // false if no read waits for that answer, which the Page refuses.
  bool Answer(uint64_t number, std::string line);

 private:
  SendFrame send_;
  base::Lock lock_;
  base::ConditionVariable answered_{&lock_};
  // The question that a read waits for the answer to, if one does.
  std::optional<uint64_t> question_ GUARDED_BY(lock_);
  std::optional<std::string> answer_ GUARDED_BY(lock_);
  // Only the main thread asks.
  uint64_t last_question_ = 0;
};

}  // namespace mitto

#endif  // MITTO_PAGE_SESSION_COOKIE_JAR_H_
