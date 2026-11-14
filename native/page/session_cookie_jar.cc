#include "mitto/page/session_cookie_jar.h"

#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/window.h"
#include "mitto/page_messages.h"

namespace mitto {

SessionCookieJar::SessionCookieJar() = default;
SessionCookieJar::~SessionCookieJar() = default;

void SessionCookieJar::SetSend(SendFrame send) {
  send_ = std::move(send);
  // Blink's CookieStore hears from RestrictedCookieManager on the DOM
  // manipulation task source.
  main_thread_ = EventLoop::Get()
                     ? EventLoop::Get()->TaskRunner(TaskType::kDOMManipulation)
                     : base::SingleThreadTaskRunner::GetCurrentDefault();
}

std::u16string SessionCookieJar::Cookies() {
  const uint64_t question = ++last_question_;
  {
    base::AutoLock lock(lock_);
    question_ = question;
    answer_.reset();
  }
  send_.Run(EncodeGetCookies(Window::Current().DocumentId(), question));
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
  if (bytes.size() > kMaxCookieLineSize) {
    return;
  }
  send_.Run(EncodeSetCookie(Window::Current().DocumentId(), bytes));
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

void SessionCookieJar::GetForCookieStore(PageCookieStoreGet get,
                                         CookiesAnswer answer) {
  // A name too long for a frame names no cookie, since //net refuses a name
  // and value of more than 4096 bytes: the Session's answer would be none,
  // and comes from a task of its own all the same.
  if (get.name.size() > kMaxPayloadSize - 17) {
    main_thread_->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(answer), std::vector<PageCookie>()));
    return;
  }
  get.question = ++last_cookie_store_question_;
  {
    base::AutoLock lock(lock_);
    cookie_store_questions_.insert(get.question);
  }
  cookie_store_gets_.emplace(get.question, std::move(answer));
  send_.Run(EncodeCookieStoreGet(Window::Current().DocumentId(), get));
}

void SessionCookieJar::SetForCookieStore(PageCookieStoreSet set,
                                         StoredAnswer answer) {
  // Nor can a cookie too long for a frame be stored: the Session's answer
  // would be that it was not.
  if (set.name.size() + set.value.size() + set.domain.size() + set.path.size() >
      kMaxPayloadSize - 38) {
    main_thread_->PostTask(FROM_HERE, base::BindOnce(std::move(answer), false));
    return;
  }
  set.question = ++last_cookie_store_question_;
  {
    base::AutoLock lock(lock_);
    cookie_store_questions_.insert(set.question);
  }
  cookie_store_sets_.emplace(set.question, std::move(answer));
  send_.Run(EncodeCookieStoreSet(Window::Current().DocumentId(), set));
}

void SessionCookieJar::ListenForCookieStore(ChangeListener listener) {
  const bool listen = !listener.is_null();
  if (listen == !cookie_store_listener_.is_null()) {
    cookie_store_listener_ = std::move(listener);
    return;
  }
  cookie_store_listener_ = std::move(listener);
  send_.Run(EncodeCookieStoreListen(Window::Current().DocumentId(), listen));
}

bool SessionCookieJar::AnswerCookieStoreGet(PageCookieStoreCookies cookies) {
  {
    base::AutoLock lock(lock_);
    if (cookie_store_questions_.erase(cookies.question) == 0) {
      return false;
    }
  }
  main_thread_->PostTask(
      FROM_HERE, base::BindOnce(
                     [](SessionCookieJar* jar, PageCookieStoreCookies cookies) {
                       const auto found =
                           jar->cookie_store_gets_.find(cookies.question);
                       if (found == jar->cookie_store_gets_.end()) {
                         return;
                       }
                       CookiesAnswer answer = std::move(found->second);
                       jar->cookie_store_gets_.erase(found);
                       std::move(answer).Run(std::move(cookies.cookies));
                     },
                     base::Unretained(this), std::move(cookies)));
  return true;
}

bool SessionCookieJar::AnswerCookieStoreSet(uint64_t number, bool stored) {
  {
    base::AutoLock lock(lock_);
    if (cookie_store_questions_.erase(number) == 0) {
      return false;
    }
  }
  main_thread_->PostTask(
      FROM_HERE, base::BindOnce(
                     [](SessionCookieJar* jar, uint64_t number, bool stored) {
                       const auto found = jar->cookie_store_sets_.find(number);
                       if (found == jar->cookie_store_sets_.end()) {
                         return;
                       }
                       StoredAnswer answer = std::move(found->second);
                       jar->cookie_store_sets_.erase(found);
                       std::move(answer).Run(stored);
                     },
                     base::Unretained(this), number, stored));
  return true;
}

void SessionCookieJar::CookieStoreChanged(PageCookieStoreChange change) {
  // A change may come after the CookieStore stopped listening, while its
  // message to stop was on the way, and then nothing hears of it.
  main_thread_->PostTask(
      FROM_HERE, base::BindOnce(
                     [](SessionCookieJar* jar, PageCookieStoreChange change) {
                       if (jar->cookie_store_listener_) {
                         jar->cookie_store_listener_.Run(change);
                       }
                     },
                     base::Unretained(this), std::move(change)));
}

}  // namespace mitto
