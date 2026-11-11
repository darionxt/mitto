#include "mitto/page/session_channel.h"

#include <stdint.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/memory/raw_ref.h"
#include "base/memory/scoped_refptr.h"
#include "base/message_loop/message_pump_type.h"
#include "base/notreached.h"
#include "base/process/process.h"
#include "base/run_loop.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/thread.h"
#include "build/build_config.h"
#include "mitto/page/script_runner.h"
#include "mitto/page_channel.h"
#include "mitto/page_messages.h"
#include "v8/include/v8-callbacks.h"
#include "v8/include/v8-isolate.h"

#if BUILDFLAG(IS_POSIX)
#include <unistd.h>
#endif

#if BUILDFLAG(IS_MAC)
#include <sys/event.h>

#include "base/files/scoped_file.h"
#include "base/message_loop/message_pump_for_io.h"
#include "base/task/current_thread.h"
#endif

namespace mitto {
namespace {

// Ends the Page process at once, from any thread, whatever its scripts are
// doing. A Page keeps nothing that has to be saved.
[[noreturn]] void EndProcess(int exit_code) {
  base::Process::TerminateCurrentProcessImmediately(exit_code);
}

// Ends the process with the code that says so, when V8 cannot get the memory
// it needs, rather than with the crash that V8 would make of it.
[[noreturn]] void OnOutOfMemory(const char* location,
                                const v8::OOMDetails& details) {
  EndProcess(kPageExitOutOfMemory);
}

// The Page's end of the channel, on the Page program's I/O thread. It hands
// each message to the main thread, which runs the scripts, and lives until the
// process ends.
class SessionLink final : public PageChannel::Delegate
#if BUILDFLAG(IS_MAC)
    ,
                          public base::MessagePumpForIO::FdWatcher
#endif
{
 public:
  SessionLink(scoped_refptr<base::SingleThreadTaskRunner> main_thread,
              ScriptRunner& runner)
      : main_thread_(std::move(main_thread)), runner_(runner) {}

  SessionLink(const SessionLink&) = delete;
  SessionLink& operator=(const SessionLink&) = delete;

  ~SessionLink() override = default;

  void Start(SessionArgument session) {
    if (!WatchParent(session.parent)) {
      // The process that holds the library has gone already.
      EndProcess(0);
    }
    channel_ = PageChannel::Start(std::move(session.channel), *this);
    if (!channel_) {
      EndProcess(kPageExitRefused);
    }
    channel_->Send(EncodeHello());
  }

  void SendToSession(std::string frame) { channel_->Send(std::move(frame)); }

  // Tells the Session why, and ends the process once it has been told.
  void Refuse(std::string why) {
    channel_->Send(EncodeFrame(PageMessage::kRefused, why));
    channel_->WhenWritten(base::BindOnce(&EndProcess, kPageExitRefused));
    refused_ = true;
  }

 private:
  // PageChannel::Delegate:
  void OnFrame(uint32_t type, std::string payload) override {
    if (refused_) {
      return;
    }
    base::expected<FrameFromSession, std::string> frame =
        ReadFrameFromSession(type, std::move(payload));
    if (!frame.has_value()) {
      Refuse(std::move(frame).error());
      return;
    }
    if (frame->type == PageMessage::kRefused) {
      // The Session refused what this sent, and closes the channel.
      EndProcess(kPageExitRefused);
    }
    if (!hello_received_) {
      if (frame->type != PageMessage::kHello) {
        Refuse(base::StrCat({"message type ", base::NumberToString(type),
                             " came before the hello"}));
        return;
      }
      if (frame->version != kPageProtocolVersion) {
        Refuse(base::StrCat({"the library speaks version ",
                             base::NumberToString(frame->version),
                             " of the messages, and the Page speaks ",
                             base::NumberToString(kPageProtocolVersion)}));
        return;
      }
      hello_received_ = true;
      return;
    }
    switch (frame->type) {
      case PageMessage::kHello:
        Refuse("a second hello");
        return;
      case PageMessage::kStart:
        if (started_) {
          Refuse("a second start");
          return;
        }
        started_ = true;
        main_thread_->PostTask(
            FROM_HERE, base::BindOnce(&SessionLink::StartOnMainThread,
                                      base::Unretained(this),
                                      base::SingleThreadTaskRunner::
                                          GetCurrentDefault(),
                                      std::move(frame->start)));
        return;
      case PageMessage::kEvaluate:
        if (!started_) {
          Refuse("a script came before the start");
          return;
        }
        main_thread_->PostTask(
            FROM_HERE, base::BindOnce(&SessionLink::EvaluateOnMainThread,
                                      base::Unretained(this), frame->call,
                                      std::move(frame->script)));
        return;
      case PageMessage::kRefused:
      case PageMessage::kEvaluated:
      case PageMessage::kConsole:
      case PageMessage::kUncaughtError:
        // Handled above, or refused by ReadFrameFromSession().
        NOTREACHED();
    }
  }

  void OnChannelClosed(PageChannel::Closed closed, std::string why) override {
    if (closed == PageChannel::Closed::kFrameTooLarge && !refused_) {
      Refuse(std::move(why));
      return;
    }
    // The Session has closed the Page, or its process has gone.
    if (!refused_) {
      EndProcess(0);
    }
  }

  // These run on the main thread, and answer on the I/O thread.

  void StartOnMainThread(scoped_refptr<base::SingleThreadTaskRunner> io_thread,
                         PageStart start) {
    // What the Page sends, which the I/O thread sends in the order it came.
    SendFrame send = base::BindRepeating(
        [](scoped_refptr<base::SingleThreadTaskRunner> io_thread,
           SessionLink* link, std::string frame) {
          io_thread->PostTask(
              FROM_HERE,
              base::BindOnce(&SessionLink::SendToSession,
                             base::Unretained(link), std::move(frame)));
        },
        io_thread, base::Unretained(this));
    if (std::optional<std::string> why =
            runner_->Start(start, std::move(send))) {
      io_thread->PostTask(FROM_HERE,
                          base::BindOnce(&SessionLink::Refuse,
                                         base::Unretained(this), *why));
    }
  }

  void EvaluateOnMainThread(uint64_t call, std::string script) {
    runner_->Evaluate(call, script);
  }

  // The end of the channel says that the process that holds the library has
  // gone, unless another process holds its end too, such as one that it forked
  // and that did not start another program. So the Page process also watches
  // that process. On Linux the library has the kernel kill the Page process
  // when it goes, and on Windows a job object does. Returns false if it has
  // gone already.
  bool WatchParent(base::ProcessId parent) {
#if BUILDFLAG(IS_POSIX)
    // The kernel may have given the Page another parent already.
    if (getppid() != parent) {
      return false;
    }
#endif
#if BUILDFLAG(IS_MAC)
    parent_watch_.reset(kqueue());
    if (!parent_watch_.is_valid()) {
      return false;
    }
    struct kevent change;
    EV_SET(&change, parent, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0,
           nullptr);
    // Watched first and asked after, so that it cannot go in between unseen.
    if (kevent(parent_watch_.get(), &change, 1, nullptr, 0, nullptr) != 0 ||
        getppid() != parent) {
      return false;
    }
    return base::CurrentIOThread::Get()->WatchFileDescriptor(
        parent_watch_.get(), /*persistent=*/false,
        base::MessagePumpForIO::WATCH_READ, &parent_controller_, this);
#else
    return true;
#endif
  }

#if BUILDFLAG(IS_MAC)
  // base::MessagePumpForIO::FdWatcher, for the end of the parent:
  void OnFileCanReadWithoutBlocking(int fd) override { EndProcess(0); }
  void OnFileCanWriteWithoutBlocking(int fd) override {}
#endif

  const scoped_refptr<base::SingleThreadTaskRunner> main_thread_;
  // Lives on the main thread, until the process ends.
  const raw_ref<ScriptRunner> runner_;
  std::unique_ptr<PageChannel> channel_;
  bool hello_received_ = false;
  bool started_ = false;
  bool refused_ = false;
#if BUILDFLAG(IS_MAC)
  base::ScopedFD parent_watch_;
  base::MessagePumpForIO::FdWatchController parent_controller_{FROM_HERE};
#endif
};

}  // namespace

SessionArgument::SessionArgument() = default;
SessionArgument::SessionArgument(SessionArgument&&) = default;
SessionArgument& SessionArgument::operator=(SessionArgument&&) = default;
SessionArgument::~SessionArgument() = default;

std::optional<SessionArgument> ParseSessionArgument(std::string_view argument) {
  if (!argument.starts_with(kSessionArgumentPrefix)) {
    return std::nullopt;
  }
  const std::vector<std::string_view> parts =
      base::SplitStringPiece(argument.substr(kSessionArgumentPrefix.size()),
                             ",", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  int64_t parent = 0;
  if (parts.size() != 2 || !base::StringToInt64(parts[1], &parent) ||
      parent <= 0) {
    return std::nullopt;
  }
  SessionArgument session;
  session.channel = PageChannelEndFromArgument(parts[0]);
  if (!session.channel.is_valid()) {
    return std::nullopt;
  }
  session.parent = static_cast<base::ProcessId>(parent);
  return session;
}

void ServeSession(v8::Platform& platform,
                  v8::Isolate* isolate,
                  SessionArgument session) {
  const base::AtExitManager at_exit_manager;
  // What //base reads of it is not on this command line.
  base::CommandLine::Init(0, nullptr);
  isolate->SetOOMErrorHandler(&OnOutOfMemory);
  base::SingleThreadTaskExecutor main_thread;
  ScriptRunner runner(platform, isolate);

  // Reads the channel while a script runs, so that the Page process ends when
  // it closes, even in the middle of a script that never ends.
  base::Thread io_thread("Page I/O");
  if (!io_thread.StartWithOptions(
          base::Thread::Options(base::MessagePumpType::IO, 0))) {
    return;
  }
  // Never destroyed: the process ends while the I/O thread uses it.
  SessionLink* const link =
      std::make_unique<SessionLink>(main_thread.task_runner(), runner)
          .release();
  io_thread.task_runner()->PostTask(
      FROM_HERE, base::BindOnce(&SessionLink::Start, base::Unretained(link),
                                std::move(session)));
  base::RunLoop().Run();
}

}  // namespace mitto
