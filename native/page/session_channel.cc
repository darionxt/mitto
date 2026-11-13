#include "mitto/page/session_channel.h"

#include <stdint.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

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
#include "base/task/single_thread_task_runner.h"
#include "base/threading/thread.h"
#include "base/types/expected.h"
#include "build/build_config.h"
#include "mitto/page/script_runner.h"
#include "mitto/page/session_cookie_jar.h"
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
              StartV8Callback start_v8,
              SessionCookieJar& cookie_jar)
      : main_thread_(std::move(main_thread)),
        start_v8_(std::move(start_v8)),
        cookie_jar_(cookie_jar) {}

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
      case PageMessage::kResponseStarted:
      case PageMessage::kResponseBody:
      case PageMessage::kResponseEnded:
        if (!started_) {
          Refuse("a response came before the start");
          return;
        }
        main_thread_->PostTask(
            FROM_HERE,
            base::BindOnce(&SessionLink::ResponseOnMainThread,
                           base::Unretained(this), *std::move(frame)));
        return;
      case PageMessage::kCookies:
        // The main thread waits for this, so it is handed over here.
        if (!cookie_jar_->Answer(frame->cookies_answer,
                                 std::move(frame->cookies))) {
          Refuse(base::StrCat({"cookies for question ",
                               base::NumberToString(frame->cookies_answer),
                               ", which no script waits for"}));
        }
        return;
      case PageMessage::kCookieStoreCookies: {
        const uint64_t question = frame->cookie_store_cookies.question;
        if (!cookie_jar_->AnswerCookieStoreGet(
                std::move(frame->cookie_store_cookies))) {
          Refuse(base::StrCat({"cookies for CookieStore question ",
                               base::NumberToString(question),
                               ", which no script waits for"}));
        }
        return;
      }
      case PageMessage::kCookieStoreStored:
        if (!cookie_jar_->AnswerCookieStoreSet(
                frame->cookie_store_stored_answer,
                frame->cookie_store_stored)) {
          Refuse(base::StrCat(
              {"an answer for CookieStore question ",
               base::NumberToString(frame->cookie_store_stored_answer),
               ", which no script waits for"}));
        }
        return;
      case PageMessage::kCookieStoreChange:
        cookie_jar_->CookieStoreChanged(std::move(frame->cookie_store_change));
        return;
      case PageMessage::kRefused:
      case PageMessage::kEvaluated:
      case PageMessage::kConsole:
      case PageMessage::kUncaughtError:
      case PageMessage::kRequest:
      case PageMessage::kCredit:
      case PageMessage::kGetCookies:
      case PageMessage::kSetCookie:
      case PageMessage::kLoaded:
      case PageMessage::kCancel:
      case PageMessage::kKnownGap:
      case PageMessage::kCookieStoreGet:
      case PageMessage::kCookieStoreSet:
      case PageMessage::kCookieStoreListen:
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
    // What the main thread refuses of what the Session sends, which the I/O
    // thread says.
    base::RepeatingCallback<void(std::string)> refuse = base::BindRepeating(
        [](scoped_refptr<base::SingleThreadTaskRunner> io_thread,
           SessionLink* link, std::string why) {
          io_thread->PostTask(FROM_HERE, base::BindOnce(&SessionLink::Refuse,
                                                        base::Unretained(link),
                                                        std::move(why)));
        },
        io_thread, base::Unretained(this));
    base::expected<v8::Isolate*, std::string> isolate =
        std::move(start_v8_).Run(start);
    if (!isolate.has_value()) {
      io_thread->PostTask(
          FROM_HERE,
          base::BindOnce(&SessionLink::Refuse, base::Unretained(this),
                         std::move(isolate).error()));
      return;
    }
    (*isolate)->SetOOMErrorHandler(&OnOutOfMemory);
    runner_ = std::make_unique<ScriptRunner>(*isolate);
    if (std::optional<std::string> why = runner_->Start(
            start, std::move(send), std::move(refuse), *cookie_jar_)) {
      io_thread->PostTask(FROM_HERE,
                          base::BindOnce(&SessionLink::Refuse,
                                         base::Unretained(this), *why));
    }
  }

  void EvaluateOnMainThread(uint64_t call, std::string script) {
    // None after a start that the Page refused, while the process ends.
    if (runner_) {
      runner_->Evaluate(call, script);
    }
  }

  void ResponseOnMainThread(FrameFromSession frame) {
    if (!runner_) {
      return;
    }
    switch (frame.type) {
      case PageMessage::kResponseStarted:
        runner_->OnResponseStarted(std::move(frame.response));
        return;
      case PageMessage::kResponseBody:
        runner_->OnResponseBody(std::move(frame.body));
        return;
      case PageMessage::kResponseEnded:
        runner_->OnResponseEnded(frame.ended);
        return;
      default:
        NOTREACHED();
    }
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
  // These are the main thread's. The runner lives from the start until the
  // process ends.
  StartV8Callback start_v8_;
  std::unique_ptr<ScriptRunner> runner_;
  // Lives until the process ends. The I/O thread hands it answers.
  const raw_ref<SessionCookieJar> cookie_jar_;
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

void ServeSession(SessionArgument session, StartV8Callback start_v8) {
  SessionCookieJar cookie_jar;

  // Reads the channel while a script runs, so that the Page process ends when
  // it closes, even in the middle of a script that never ends.
  base::Thread io_thread("Page I/O");
  if (!io_thread.StartWithOptions(
          base::Thread::Options(base::MessagePumpType::IO, 0))) {
    return;
  }
  // Never destroyed: the process ends while the I/O thread uses it.
  SessionLink* const link =
      std::make_unique<SessionLink>(
          base::SingleThreadTaskRunner::GetCurrentDefault(),
          std::move(start_v8), cookie_jar)
          .release();
  io_thread.task_runner()->PostTask(
      FROM_HERE, base::BindOnce(&SessionLink::Start, base::Unretained(link),
                                std::move(session)));
  base::RunLoop().Run();
}

}  // namespace mitto
