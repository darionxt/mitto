#include "mitto/page/session_cookie_jar.h"

#include <utility>

#include "base/strings/utf_string_conversions.h"
#include "mitto/page_messages.h"

namespace mitto {

SessionCookieJar::SessionCookieJar() = default;
SessionCookieJar::~SessionCookieJar() = default;

void SessionCookieJar::SetSend(SendFrame send) {
  send_ = std::move(send);
}

std::u16string SessionCookieJar::Cookies() {
  const uint64_t question = ++last_question_;
  {
    base::AutoLock lock(lock_);
    question_ = question;
    answer_.reset();
  }
  send_.Run(EncodeGetCookies(question));
  std::string line;
  {
    base::AutoLock lock(lock_);
    // The process ends when the channel closes, so this cannot wait forever.
    while (!answer_) {
      answered_.Wait();
    }
    line = *std::move(answer_);
    answer_.reset();
    question_.reset();
  }
  // Blink reads the line of its Mojo call as UTF-8.
  return base::UTF8ToUTF16(line);
}

void SessionCookieJar::SetCookie(const std::u16string& line) {
  std::string bytes = base::UTF16ToUTF8(line);
  // //net refuses a cookie line far shorter than a frame holds.
  if (bytes.size() > kMaxPayloadSize) {
    return;
  }
  send_.Run(EncodeSetCookie(bytes));
}

bool SessionCookieJar::Answer(uint64_t number, std::string line) {
  base::AutoLock lock(lock_);
  if (question_ != number || answer_) {
    return false;
  }
  answer_ = std::move(line);
  answered_.Signal();
  return true;
}

}  // namespace mitto
