// The Page program of the channel prototype. It answers its Session over the
// channel it inherits, opens nothing else, and ends when the channel closes or
// its parent dies. Nothing of it merges.

#include <stdint.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/environment.h"
#include "base/files/scoped_file.h"
#include "base/functional/bind.h"
#include "base/immediate_crash.h"
#include "base/location.h"
#include "base/memory/weak_ptr.h"
#include "base/message_loop/message_pump_for_io.h"
#include "base/message_loop/message_pump_type.h"
#include "base/process/process.h"
#include "base/process/process_handle.h"
#include "base/run_loop.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/current_thread.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "mitto/page_channel.h"

#if BUILDFLAG(IS_MAC)
#include <sys/event.h>
#include <unistd.h>
#endif

namespace mitto {
namespace {

// How long the Page takes to read each part of a body, so that the Session runs
// out of credit and has to wait.
constexpr base::TimeDelta kReadDelay = base::Milliseconds(1);

class PageProcess final : public ChannelEnd::Delegate,
                          public base::MessagePumpForIO::FdWatcher {
 public:
  explicit PageProcess(uint32_t version) : version_(version) {}

  PageProcess(const PageProcess&) = delete;
  PageProcess& operator=(const PageProcess&) = delete;

  ~PageProcess() override = default;

  bool Start(base::ScopedFD channel, base::ProcessId parent) {
    channel_ = std::make_unique<ChannelEnd>(std::move(channel), this);
    if (!channel_->Start()) {
      return false;
    }
    channel_->Send(PageMessage::kHello, EncodeUint32(version_));
    return WatchParent(parent);
  }

 private:
  // ChannelEnd::Delegate:
  void OnFrame(uint32_t type, std::string payload) override {
    if (!IsKnownPageMessage(type)) {
      Refuse(base::StrCat({"message type ", base::NumberToString(type),
                           " is unknown to this Page"}));
      return;
    }
    const auto message = static_cast<PageMessage>(type);
    if (!hello_received_ && message != PageMessage::kHello &&
        message != PageMessage::kRefused) {
      Refuse("the Session did not say hello first");
      return;
    }
    switch (message) {
      case PageMessage::kHello: {
        uint32_t version = 0;
        if (!DecodeUint32(payload, version) || version != version_) {
          Refuse(base::StrCat({"the Session speaks protocol version ",
                               base::NumberToString(version),
                               " and this Page speaks ",
                               base::NumberToString(version_)}));
          return;
        }
        hello_received_ = true;
        return;
      }
      case PageMessage::kEvaluate:
        Evaluate(payload);
        return;
      case PageMessage::kBodyStart: {
        uint64_t size = 0;
        if (!DecodeUint64(payload, size) || body_open_) {
          Refuse("a body started that cannot start");
          return;
        }
        body_open_ = true;
        body_ended_ = false;
        body_size_ = size;
        body_received_ = 0;
        body_held_ = 0;
        body_most_held_ = 0;
        credit_left_ = kBodyWindow;
        credits_sent_ = 1;
        channel_->Send(PageMessage::kBodyCredit, EncodeUint32(kBodyWindow));
        return;
      }
      case PageMessage::kBodyData: {
        if (!body_open_ || payload.size() > credit_left_) {
          Refuse("the Session sent more of a body than it had credit for");
          return;
        }
        credit_left_ -= payload.size();
        body_received_ += payload.size();
        body_held_ += payload.size();
        body_most_held_ = std::max(body_most_held_, body_held_);
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
            FROM_HERE,
            base::BindOnce(&PageProcess::ReadBody, weak_factory_.GetWeakPtr(),
                           static_cast<uint32_t>(payload.size())),
            kReadDelay);
        return;
      }
      case PageMessage::kBodyEnd:
        body_ended_ = true;
        MaybeFinishBody();
        return;
      case PageMessage::kRefused:
        base::Process::TerminateCurrentProcessImmediately(0);
      case PageMessage::kResult:
      case PageMessage::kBodyCredit:
      case PageMessage::kBodyDone:
        Refuse(base::StrCat({"message type ", base::NumberToString(type),
                             " only goes to a Session"}));
        return;
    }
  }

  void OnChannelClosed(std::string_view why) override {
    // The Session has closed the Page or gone.
    base::Process::TerminateCurrentProcessImmediately(0);
  }

  // base::MessagePumpForIO::FdWatcher, for the parent's exit:
  void OnFileCanReadWithoutBlocking(int fd) override {
    base::Process::TerminateCurrentProcessImmediately(0);
  }
  void OnFileCanWriteWithoutBlocking(int fd) override {}

  // The channel's end of stream says that the parent is gone, unless another
  // process holds the parent's end too, such as a child that the parent forked
  // and did not exec. So the Page watches its parent as well. Linux sends
  // the Page a signal instead, and Windows ends it with a job object.
  bool WatchParent(base::ProcessId parent) {
#if BUILDFLAG(IS_MAC)
    parent_watch_.reset(kqueue());
    if (!parent_watch_.is_valid()) {
      return false;
    }
    struct kevent change;
    EV_SET(&change, parent, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0,
           nullptr);
    if (kevent(parent_watch_.get(), &change, 1, nullptr, 0, nullptr) != 0 ||
        getppid() != parent) {
      // The parent has already gone.
      return false;
    }
    return base::CurrentIOThread::Get()->WatchFileDescriptor(
        parent_watch_.get(), /*persistent=*/false,
        base::MessagePumpForIO::WATCH_READ, &parent_controller_, this);
#else
    return true;
#endif
  }

  void Evaluate(std::string_view script) {
    if (script == "send-unknown") {
      channel_->SendUnknownForTest(0x7778);
      return;
    }
    if (script == "crash") {
      base::ImmediateCrash();
    }
    channel_->Send(
        PageMessage::kResult,
        base::StrCat({"process ",
                      base::NumberToString(base::GetCurrentProcId()),
                      " evaluated \"", script, "\""}));
  }

  void ReadBody(uint32_t size) {
    body_held_ -= size;
    credit_left_ += size;
    ++credits_sent_;
    channel_->Send(PageMessage::kBodyCredit, EncodeUint32(size));
    MaybeFinishBody();
  }

  void MaybeFinishBody() {
    if (!body_open_ || !body_ended_ || body_held_ != 0) {
      return;
    }
    body_open_ = false;
    channel_->Send(
        PageMessage::kBodyDone,
        base::StrCat({"the Page read ", base::NumberToString(body_received_),
                      " of ", base::NumberToString(body_size_),
                      " bytes, held at most ",
                      base::NumberToString(body_most_held_), " at once (window ",
                      base::NumberToString(kBodyWindow), "), gave credit ",
                      base::NumberToString(credits_sent_), " times"}));
  }

  void Refuse(std::string why) {
    // The frame is in the Session's socket buffer before the process ends, so
    // it still arrives.
    channel_->Send(PageMessage::kRefused, why);
    base::Process::TerminateCurrentProcessImmediately(1);
  }

  const uint32_t version_;
  std::unique_ptr<ChannelEnd> channel_;
  bool hello_received_ = false;

  bool body_open_ = false;
  bool body_ended_ = false;
  uint64_t body_size_ = 0;
  uint64_t body_received_ = 0;
  uint64_t body_held_ = 0;
  uint64_t body_most_held_ = 0;
  uint64_t credit_left_ = 0;
  int credits_sent_ = 0;

#if BUILDFLAG(IS_MAC)
  base::ScopedFD parent_watch_;
  base::MessagePumpForIO::FdWatchController parent_controller_{FROM_HERE};
#endif

  base::WeakPtrFactory<PageProcess> weak_factory_{this};
};

}  // namespace
}  // namespace mitto

int main(int argc, char** argv) {
  base::AtExitManager at_exit_manager;
  base::CommandLine::Init(argc, argv);
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  int parent = 0;
  if (!base::StringToInt(command_line.GetSwitchValueASCII("parent-pid"),
                         &parent)) {
    return 2;
  }
  uint32_t version = mitto::kPageProtocolVersion;
  // Lets the prototype's test start a Page that speaks another version.
  if (std::optional<std::string> forced =
          base::Environment::Create()->GetVar("MITTO_PAGE_PROTOTYPE_VERSION")) {
    if (!base::StringToUint(*forced, &version)) {
      return 2;
    }
  }

  base::SingleThreadTaskExecutor executor(base::MessagePumpType::IO);
  mitto::PageProcess page(version);
  if (!page.Start(base::ScopedFD(mitto::kPageChannelDescriptor), parent)) {
    return 1;
  }
  base::RunLoop().Run();
  return 0;
}
