#include "mitto/page_host.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/location.h"
#include "base/notreached.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "mitto/mitto.h"
#include "mitto/ready_requests.h"
#include "net/base/net_errors.h"

namespace mitto {
namespace {

constexpr std::string_view kVersionKey = "version";
constexpr std::string_view kTimeZoneKey = "time_zone";
constexpr std::string_view kLocaleKey = "locale";

// What a Page reports when the caller names nothing.
constexpr std::string_view kDefaultTimeZone = "UTC";
constexpr std::string_view kDefaultLocale = "en-US";

// Names of time zones and tags of locales are short and ASCII. Which ones exist
// only the Page knows, because only it holds ICU's whole data.
constexpr size_t kMaxNameSize = 64;

// The levels of the messages and of the C API are the same numbers.
static_assert(static_cast<int>(ConsoleLevel::kDebug) ==
              MITTO_PAGE_CONSOLE_DEBUG);
static_assert(static_cast<int>(ConsoleLevel::kLog) == MITTO_PAGE_CONSOLE_LOG);
static_assert(static_cast<int>(ConsoleLevel::kInfo) == MITTO_PAGE_CONSOLE_INFO);
static_assert(static_cast<int>(ConsoleLevel::kWarning) ==
              MITTO_PAGE_CONSOLE_WARNING);
static_assert(static_cast<int>(ConsoleLevel::kError) ==
              MITTO_PAGE_CONSOLE_ERROR);

bool IsReport(const PageEvent& event) {
  return event.type == MITTO_PAGE_EVENT_CONSOLE ||
         event.type == MITTO_PAGE_EVENT_UNCAUGHT_ERROR;
}

bool IsName(std::string_view name) {
  return !name.empty() && name.size() <= kMaxNameSize &&
         std::ranges::all_of(name, [](char c) {
           return base::IsAsciiAlphaNumeric(c) || c == '_' || c == '-' ||
                  c == '+' || c == '/';
         });
}

}  // namespace

std::optional<PageStart> ParsePageOptions(std::string_view json) {
  const std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!dict || dict->FindInt(kVersionKey) != MITTO_PAGE_OPTIONS_VERSION) {
    return std::nullopt;
  }
  PageStart start;
  start.time_zone = kDefaultTimeZone;
  start.locale = kDefaultLocale;
  for (const auto [key, value] : *dict) {
    if (key == kVersionKey) {
      continue;
    }
    std::string* const name = key == kTimeZoneKey ? &start.time_zone
                              : key == kLocaleKey ? &start.locale
                                                  : nullptr;
    if (!name || !value.is_string() || !IsName(value.GetString())) {
      return std::nullopt;
    }
    *name = value.GetString();
  }
  return start;
}

PageEvent::PageEvent() = default;
PageEvent::PageEvent(PageEvent&&) = default;
PageEvent& PageEvent::operator=(PageEvent&&) = default;
PageEvent::~PageEvent() = default;

PageEvents::PageEvents() = default;
PageEvents::~PageEvents() = default;

uint64_t PageEvents::NewCall() {
  base::AutoLock lock(lock_);
  return next_call_++;
}

int PageEvents::NextEvent(std::optional<base::TimeDelta> timeout,
                          PageEvent& event) {
  base::AutoLock lock(lock_);
  if (timeout) {
    const base::TimeTicks deadline = base::TimeTicks::Now() + *timeout;
    while (events_.empty() && !waits_ended_) {
      const base::TimeDelta left = deadline - base::TimeTicks::Now();
      if (!left.is_positive()) {
        return net::ERR_IO_PENDING;
      }
      event_waiting_.TimedWait(left);
    }
  } else {
    while (events_.empty() && !waits_ended_) {
      event_waiting_.Wait();
    }
  }
  if (events_.empty()) {
    return net::ERR_IO_PENDING;
  }
  event = std::move(events_.front());
  events_.pop_front();
  if (IsReport(event)) {
    --waiting_reports_;
    waiting_report_bytes_ -= event.message.size();
  }
  return net::OK;
}

void PageEvents::EndWaits() {
  base::AutoLock lock(lock_);
  waits_ended_ = true;
  event_waiting_.Broadcast();
}

void PageEvents::Push(PageEvent event) {
  base::AutoLock lock(lock_);
  events_.push_back(std::move(event));
  event_waiting_.Signal();
}

int PageEvents::PushReport(PageEvent event) {
  base::AutoLock lock(lock_);
  std::optional<PageEvent> note;
  if (dropped_reports_ > 0) {
    note.emplace();
    note->type = MITTO_PAGE_EVENT_CONSOLE;
    note->level = MITTO_PAGE_CONSOLE_WARNING;
    note->message = base::StrCat(
        {"the library dropped ", base::NumberToString(dropped_reports_),
         " console messages and uncaught errors of this Page, because too "
         "many waited untaken"});
  }
  const size_t pushed = note ? 2 : 1;
  const size_t bytes =
      event.message.size() + (note ? note->message.size() : 0);
  // Neither sum can wrap: what waits stays within the limits, and one
  // message is at most a frame.
  if (waiting_reports_ + pushed > kMaxWaitingReports ||
      waiting_report_bytes_ + bytes > kMaxWaitingReportBytes) {
    ++dropped_reports_;
    return 0;
  }
  if (note) {
    AddReport(*std::move(note));
    dropped_reports_ = 0;
  }
  AddReport(std::move(event));
  event_waiting_.Signal();
  return static_cast<int>(pushed);
}

void PageEvents::AddReport(PageEvent event) {
  ++waiting_reports_;
  waiting_report_bytes_ += event.message.size();
  events_.push_back(std::move(event));
}

void PageEvents::PushEnded(int32_t error, std::string message) {
  base::AutoLock lock(lock_);
  ended_error_ = error;
  ended_message_ = message;
  PageEvent event;
  event.type = MITTO_PAGE_EVENT_ENDED;
  event.error = error;
  event.message = std::move(message);
  events_.push_back(std::move(event));
  event_waiting_.Signal();
}

void PageEvents::PushCallAfterEnd(uint64_t call) {
  base::AutoLock lock(lock_);
  PageEvent event;
  event.type = MITTO_PAGE_EVENT_EVALUATED;
  event.error = ended_error_;
  event.call = call;
  event.message = ended_message_;
  events_.push_back(std::move(event));
  event_waiting_.Signal();
}

PageHost::PageHost(scoped_refptr<PageEvents> events,
                   scoped_refptr<PageProcess> process,
                   ReadyRequests& ready,
                   uintptr_t handle)
    : events_(std::move(events)),
      process_(std::move(process)),
      ready_(ready),
      handle_(handle) {}

PageHost::~PageHost() = default;

bool PageHost::Start(PageChannelEnd ours, const PageStart& start) {
  channel_ = PageChannel::Start(std::move(ours), *this);
  if (!channel_) {
    return false;
  }
  // The Page reads these in order, so nothing waits for its hello.
  channel_->Send(EncodeHello());
  channel_->Send(EncodeStart(start));
  return true;
}

void PageHost::Evaluate(uint64_t call, std::string script) {
  if (ended_) {
    events_->PushCallAfterEnd(call);
    ready_->Add(handle_, MITTO_READY_PAGE);
    return;
  }
  calls_.insert(call);
  // Once the channel has closed, the call fails when the Page ends.
  if (channel_) {
    channel_->Send(EncodeEvaluate(call, script));
  }
}

void PageHost::EndWithSession() {
  if (ended_) {
    return;
  }
  channel_.reset();
  End(MITTO_PAGE_ERROR_SESSION_DESTROYED, "the Session was destroyed");
}

void PageHost::OnFrame(uint32_t type, std::string payload) {
  base::expected<FrameFromPage, std::string> frame =
      ReadFrameFromPage(type, std::move(payload));
  if (!frame.has_value()) {
    Refuse(std::move(frame).error());
    return;
  }
  if (!hello_received_ && frame->type != PageMessage::kHello &&
      frame->type != PageMessage::kRefused) {
    Refuse(base::StrCat({"message type ", base::NumberToString(type),
                         " came before the hello"}));
    return;
  }
  switch (frame->type) {
    case PageMessage::kHello:
      if (hello_received_) {
        Refuse("a second hello");
        return;
      }
      if (frame->version != kPageProtocolVersion) {
        Refuse(base::StrCat({"the Page speaks version ",
                             base::NumberToString(frame->version),
                             " of the messages, and the library speaks ",
                             base::NumberToString(kPageProtocolVersion)}));
        return;
      }
      hello_received_ = true;
      return;
    case PageMessage::kRefused:
      // The Page ends after this.
      refused_by_page_ = std::move(frame->reason);
      WaitForExit();
      return;
    case PageMessage::kEvaluated: {
      PageEvaluated& evaluated = frame->evaluated;
      if (calls_.erase(evaluated.call) == 0) {
        Refuse(base::StrCat({"a result of call ",
                             base::NumberToString(evaluated.call),
                             ", which is not in progress"}));
        return;
      }
      PageEvent event;
      event.type = MITTO_PAGE_EVENT_EVALUATED;
      event.call = evaluated.call;
      if (evaluated.failed) {
        event.error = MITTO_PAGE_ERROR_SCRIPT;
        event.message = std::move(evaluated.text);
      } else {
        event.json = std::move(evaluated.text);
      }
      Push(std::move(event));
      return;
    }
    case PageMessage::kConsole: {
      PageEvent event;
      event.type = MITTO_PAGE_EVENT_CONSOLE;
      event.level = static_cast<int32_t>(frame->console.level);
      event.message = std::move(frame->console.text);
      PushReport(std::move(event));
      return;
    }
    case PageMessage::kUncaughtError: {
      PageEvent event;
      event.type = MITTO_PAGE_EVENT_UNCAUGHT_ERROR;
      event.error = MITTO_PAGE_ERROR_SCRIPT;
      event.message = std::move(frame->uncaught_error);
      PushReport(std::move(event));
      return;
    }
    case PageMessage::kStart:
    case PageMessage::kEvaluate:
      // ReadFrameFromPage() refuses these.
      NOTREACHED();
  }
}

void PageHost::OnChannelClosed(PageChannel::Closed closed, std::string why) {
  if (closed == PageChannel::Closed::kFrameTooLarge) {
    Refuse(std::move(why));
    return;
  }
  WaitForExit();
}

void PageHost::Refuse(std::string why) {
  if (channel_) {
    channel_->Send(EncodeFrame(PageMessage::kRefused, why));
  }
  refused_by_library_ = std::move(why);
  process_->Kill();
  WaitForExit();
}

void PageHost::WaitForExit() {
  // What was sent already still reaches the Page, if it reads it.
  channel_.reset();
  if (waiting_for_exit_ || ended_) {
    return;
  }
  waiting_for_exit_ = true;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::WithBaseSyncPrimitives()},
      base::BindOnce(&PageProcess::WaitForExit, process_, kPageExitGrace),
      base::BindOnce(&PageHost::OnExited, weak_factory_.GetWeakPtr()));
}

void PageHost::OnExited(std::string how) {
  if (ended_) {
    return;
  }
  if (refused_by_library_) {
    End(MITTO_PAGE_ERROR_REFUSED,
        base::StrCat({"the library refused a message from the Page: ",
                      *refused_by_library_}));
  } else if (refused_by_page_) {
    End(MITTO_PAGE_ERROR_REFUSED,
        base::StrCat({"the Page refused a message from the library: ",
                      *refused_by_page_}));
  } else {
    End(MITTO_PAGE_ERROR_PROCESS_ENDED,
        base::StrCat({"the Page process ", how}));
  }
}

void PageHost::End(int32_t error, std::string message) {
  ended_ = true;
  for (const uint64_t call : calls_) {
    PageEvent event;
    event.type = MITTO_PAGE_EVENT_EVALUATED;
    event.error = error;
    event.call = call;
    event.message = message;
    Push(std::move(event));
  }
  calls_.clear();
  events_->PushEnded(error, std::move(message));
  ready_->Add(handle_, MITTO_READY_PAGE);
}

void PageHost::Push(PageEvent event) {
  events_->Push(std::move(event));
  ready_->Add(handle_, MITTO_READY_PAGE);
}

void PageHost::PushReport(PageEvent event) {
  // The list names a Page once for each of its events.
  for (int pushed = events_->PushReport(std::move(event)); pushed > 0;
       --pushed) {
    ready_->Add(handle_, MITTO_READY_PAGE);
  }
}

}  // namespace mitto
