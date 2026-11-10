#include "mitto/page_host.h"

#include <algorithm>
#include <utility>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/posix/eintr_wrapper.h"
#include "base/process/launch.h"
#include "base/process/process_handle.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/thread_pool.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "mitto/mitto.h"
#include "mitto/ready_requests.h"

#if BUILDFLAG(IS_POSIX)
#include <signal.h>
#include <sys/wait.h>
#endif

namespace mitto {

namespace {

// How long a Page process has to end once its channel has closed, before it is
// killed.
constexpr base::TimeDelta kExitTimeout = base::Seconds(2);

#if BUILDFLAG(IS_POSIX)
std::string DescribeWaitStatus(int status) {
  if (WIFSIGNALED(status)) {
    return base::StrCat(
        {"was ended by signal ", base::NumberToString(WTERMSIG(status))});
  }
  return base::StrCat(
      {"exited with ", base::NumberToString(WEXITSTATUS(status))});
}

// Reaps the process, which is ending or has ended, and kills it if it does not
// end in time. Returns what became of it.
std::string WaitForPageExit(base::ProcessId pid) {
  const base::TimeTicks deadline = base::TimeTicks::Now() + kExitTimeout;
  int status = 0;
  while (base::TimeTicks::Now() < deadline) {
    const pid_t result = HANDLE_EINTR(waitpid(pid, &status, WNOHANG));
    if (result == pid) {
      return DescribeWaitStatus(status);
    }
    if (result < 0) {
      return "could not be waited for";
    }
    base::PlatformThread::Sleep(base::Milliseconds(5));
  }
  kill(pid, SIGKILL);
  if (HANDLE_EINTR(waitpid(pid, &status, 0)) == pid) {
    return base::StrCat({"did not end in time and ", DescribeWaitStatus(status)});
  }
  return "did not end in time and could not be waited for";
}
#endif

}  // namespace

PageEvents::PageEvents() = default;
PageEvents::~PageEvents() = default;

void PageEvents::Add(PageEvent event) {
  base::AutoLock lock(lock_);
  events_.push_back(std::move(event));
}

std::optional<PageEvent> PageEvents::Take() {
  base::AutoLock lock(lock_);
  if (events_.empty()) {
    return std::nullopt;
  }
  PageEvent event = std::move(events_.front());
  events_.pop_front();
  return event;
}

PageHost::PageHost(scoped_refptr<PageEvents> events,
                   ReadyRequests& ready,
                   uintptr_t handle)
    : events_(std::move(events)), ready_(ready), handle_(handle) {}

PageHost::~PageHost() {
  if (channel_) {
    channel_->Shutdown();
  }
  if (process_.IsValid() && !reaping_) {
    base::ThreadPool::PostTask(
        FROM_HERE, {base::MayBlock(), base::WithBaseSyncPrimitives()},
        base::BindOnce(base::IgnoreResult(&WaitForPageExit), process_.Pid()));
  }
}

std::optional<int64_t> PageHost::Launch(const base::FilePath& program) {
  base::ScopedFD ours;
  base::ScopedFD theirs;
  if (!CreateChannelSockets(ours, theirs)) {
    return std::nullopt;
  }
  base::LaunchOptions options;
  options.fds_to_remap.emplace_back(theirs.get(), kPageChannelDescriptor);
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
  // The kernel sends the signal when the thread that started the child ends,
  // so this has to run on a thread that lives as long as the process, as the
  // network thread does.
  options.kill_on_parent_death = true;
#endif
  base::CommandLine command_line(program);
  command_line.AppendSwitchASCII(
      "parent-pid", base::NumberToString(base::GetCurrentProcId()));
  process_ = base::LaunchProcess(command_line, options);
  theirs.reset();
  if (!process_.IsValid()) {
    return std::nullopt;
  }
  channel_ = std::make_unique<ChannelEnd>(std::move(ours), this);
  if (!channel_->Start()) {
    channel_.reset();
    return std::nullopt;
  }
  channel_->Send(PageMessage::kHello, EncodeUint32(kPageProtocolVersion));
  return process_.Pid();
}

void PageHost::Evaluate(std::string script) {
  if (channel_ && channel_->is_open()) {
    channel_->Send(PageMessage::kEvaluate, script);
  }
}

void PageHost::StreamBody(uint64_t size) {
  if (!channel_ || !channel_->is_open() || body_open_) {
    return;
  }
  body_open_ = true;
  body_left_ = size;
  body_credit_ = 0;
  body_stalls_ = 0;
  channel_->Send(PageMessage::kBodyStart, EncodeUint64(size));
}

void PageHost::SendUnknownForTest() {
  if (channel_ && channel_->is_open()) {
    channel_->SendUnknownForTest(0x7777);
  }
}

void PageHost::OnFrame(uint32_t type, std::string payload) {
  if (!IsKnownPageMessage(type)) {
    Refuse(base::StrCat({"the Page sent message type ",
                         base::NumberToString(type),
                         ", which this library does not know"}));
    return;
  }
  const auto message = static_cast<PageMessage>(type);
  if (!hello_received_ && message != PageMessage::kHello &&
      message != PageMessage::kRefused) {
    Refuse("the Page did not say hello first");
    return;
  }
  switch (message) {
    case PageMessage::kHello: {
      uint32_t version = 0;
      if (!DecodeUint32(payload, version) || version != kPageProtocolVersion) {
        Refuse(base::StrCat({"the Page speaks protocol version ",
                             base::NumberToString(version),
                             " and this library speaks ",
                             base::NumberToString(kPageProtocolVersion)}));
        return;
      }
      hello_received_ = true;
      Emit(MITTO_PAGE_EVENT_READY, "");
      return;
    }
    case PageMessage::kResult:
      Emit(MITTO_PAGE_EVENT_RESULT, std::move(payload));
      return;
    case PageMessage::kBodyCredit: {
      uint32_t credit = 0;
      if (!DecodeUint32(payload, credit)) {
        Refuse("the Page sent credit that is not 4 bytes");
        return;
      }
      body_credit_ += credit;
      PumpBody();
      return;
    }
    case PageMessage::kBodyDone:
      Emit(MITTO_PAGE_EVENT_BODY_DONE,
           base::StrCat({payload, "; the library waited for credit ",
                         base::NumberToString(body_stalls_), " times"}));
      return;
    case PageMessage::kRefused:
      Emit(MITTO_PAGE_EVENT_REFUSED,
           base::StrCat({"the Page refused: ", payload}));
      // The Page closes the channel, and OnChannelClosed() follows.
      return;
    case PageMessage::kEvaluate:
    case PageMessage::kBodyStart:
    case PageMessage::kBodyData:
    case PageMessage::kBodyEnd:
      Refuse(base::StrCat({"the Page sent message type ",
                           base::NumberToString(type),
                           ", which only goes to a Page"}));
      return;
  }
}

void PageHost::OnChannelClosed(std::string_view why) {
  WatchForExit(std::string(why));
}

void PageHost::Emit(int32_t type, std::string text) {
  events_->Add({type, std::move(text)});
  ready_->Add(handle_);
}

void PageHost::Refuse(std::string why) {
  Emit(MITTO_PAGE_EVENT_REFUSED, base::StrCat({"the library refused: ", why}));
  channel_->Send(PageMessage::kRefused, why);
  channel_->Shutdown();
  WatchForExit("the library closed the channel");
}

void PageHost::PumpBody() {
  while (body_open_ && body_left_ > 0) {
    if (body_credit_ == 0) {
      // A real body would not read from the network now, so the server slows.
      ++body_stalls_;
      return;
    }
    const uint64_t chunk =
        std::min({uint64_t{kBodyChunkSize}, body_left_, body_credit_});
    channel_->Send(PageMessage::kBodyData,
                   std::string(static_cast<size_t>(chunk), 'x'));
    body_left_ -= chunk;
    body_credit_ -= chunk;
  }
  if (body_open_) {
    body_open_ = false;
    channel_->Send(PageMessage::kBodyEnd, "");
  }
}

void PageHost::WatchForExit(std::string why) {
  if (reaping_ || !process_.IsValid()) {
    return;
  }
  reaping_ = true;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::WithBaseSyncPrimitives()},
      base::BindOnce(&WaitForPageExit, process_.Pid()),
      base::BindOnce(&PageHost::OnExited, weak_factory_.GetWeakPtr(),
                     std::move(why)));
}

void PageHost::OnExited(std::string why, std::string status) {
  Emit(MITTO_PAGE_EVENT_GONE,
       base::StrCat({why, "; the Page process ", status}));
}

}  // namespace mitto
