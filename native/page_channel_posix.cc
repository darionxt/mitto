#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/message_loop/message_pump_for_io.h"
#include "base/posix/eintr_wrapper.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/current_thread.h"
#include "base/task/single_thread_task_runner.h"
#include "build/build_config.h"
#include "mitto/page_channel.h"
#include "mitto/page_messages.h"

namespace mitto {
namespace {

// A write to a socket whose reader has gone fails, rather than raising SIGPIPE,
// which would end the process. macOS has no such flag, so CreatePageChannel()
// sets SO_NOSIGPIPE on both sockets.
#if BUILDFLAG(IS_APPLE)
constexpr int kSendFlags = 0;
#else
constexpr int kSendFlags = MSG_NOSIGNAL;
#endif

// How much one read takes from the socket at most.
constexpr size_t kReadSize = 64 * 1024;

class PageChannelPosix final : public PageChannel,
                               public base::MessagePumpForIO::FdWatcher {
 public:
  PageChannelPosix(base::ScopedFD socket, Delegate& delegate)
      : socket_(std::move(socket)), delegate_(delegate) {}

  ~PageChannelPosix() override = default;

  bool Start() {
    return base::CurrentIOThread::Get()->WatchFileDescriptor(
        socket_.get(), /*persistent=*/true, base::MessagePumpForIO::WATCH_READ,
        &read_controller_, this);
  }

  // PageChannel:
  void Send(std::string frame) override {
    if (!writable_) {
      return;
    }
    unwritten_.append(frame);
    Flush();
  }

  void WhenWritten(base::OnceClosure done) override {
    written_ = std::move(done);
    RunWrittenIfDone();
  }

 private:
  // base::MessagePumpForIO::FdWatcher:
  void OnFileCanReadWithoutBlocking(int fd) override {
    const ssize_t read = HANDLE_EINTR(
        recv(socket_.get(), read_buffer_.data(), read_buffer_.size(), 0));
    if (read == 0) {
      Close(Closed::kByOtherEnd, "the other end closed the channel");
      return;
    }
    if (read < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
      }
      if (errno == ECONNRESET) {
        Close(Closed::kByOtherEnd, "the other end closed the channel");
        return;
      }
      Close(Closed::kFailed, base::StrCat({"reading the channel failed: errno ",
                                           base::NumberToString(errno)}));
      return;
    }
    reader_.Append(base::span(read_buffer_).first(static_cast<size_t>(read)));
    const base::WeakPtr<PageChannelPosix> alive = weak_factory_.GetWeakPtr();
    uint32_t type = 0;
    std::string payload;
    size_t size = 0;
    while (true) {
      switch (reader_.Next(type, payload, size)) {
        case FrameReader::Result::kNeedMore:
          return;
        case FrameReader::Result::kTooLarge:
          read_controller_.StopWatchingFileDescriptor();
          delegate_->OnChannelClosed(
              Closed::kFrameTooLarge,
              base::StrCat({"a frame of ", base::NumberToString(size),
                            " bytes is larger than the channel carries"}));
          return;
        case FrameReader::Result::kFrame:
          delegate_->OnFrame(type, std::move(payload));
          // The delegate may have destroyed the channel.
          if (!alive || !readable_) {
            return;
          }
          break;
      }
    }
  }

  void OnFileCanWriteWithoutBlocking(int fd) override { Flush(); }

  void Flush() {
    while (!unwritten_.empty()) {
      const ssize_t written =
          HANDLE_EINTR(send(socket_.get(), unwritten_.data(),
                            unwritten_.size(), kSendFlags));
      if (written < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          if (!watching_write_) {
            watching_write_ = base::CurrentIOThread::Get()->WatchFileDescriptor(
                socket_.get(), /*persistent=*/true,
                base::MessagePumpForIO::WATCH_WRITE, &write_controller_, this);
          }
          return;
        }
        // The other end has gone. What it sent before still waits to be read,
        // and the end of the stream that follows says that it has gone.
        writable_ = false;
        unwritten_.clear();
        StopWatchingWrite();
        RunWrittenIfDone();
        return;
      }
      unwritten_.erase(0, static_cast<size_t>(written));
    }
    StopWatchingWrite();
    RunWrittenIfDone();
  }

  void RunWrittenIfDone() {
    if (written_ && (unwritten_.empty() || !writable_)) {
      // From a task of its own, because it may destroy the channel.
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, std::move(written_));
    }
  }

  void StopWatchingWrite() {
    if (watching_write_) {
      write_controller_.StopWatchingFileDescriptor();
      watching_write_ = false;
    }
  }

  void Close(Closed closed, std::string why) {
    readable_ = false;
    writable_ = false;
    read_controller_.StopWatchingFileDescriptor();
    StopWatchingWrite();
    unwritten_.clear();
    RunWrittenIfDone();
    delegate_->OnChannelClosed(closed, std::move(why));
  }

  const base::ScopedFD socket_;
  const raw_ref<Delegate> delegate_;
  base::MessagePumpForIO::FdWatchController read_controller_{FROM_HERE};
  base::MessagePumpForIO::FdWatchController write_controller_{FROM_HERE};
  bool readable_ = true;
  bool writable_ = true;
  bool watching_write_ = false;
  std::vector<uint8_t> read_buffer_ = std::vector<uint8_t>(kReadSize);
  FrameReader reader_;
  std::string unwritten_;
  base::OnceClosure written_;
  base::WeakPtrFactory<PageChannelPosix> weak_factory_{this};
};

}  // namespace

// static
std::unique_ptr<PageChannel> PageChannel::Start(PageChannelEnd end,
                                                Delegate& delegate) {
  if (!end.is_valid() || !base::SetNonBlocking(end.get())) {
    return nullptr;
  }
  auto channel = std::make_unique<PageChannelPosix>(std::move(end), delegate);
  if (!channel->Start()) {
    return nullptr;
  }
  return channel;
}

bool CreatePageChannel(PageChannelEnd& ours, PageChannelEnd& theirs) {
  int sockets[2];
#if BUILDFLAG(IS_APPLE)
  // macOS has no SOCK_CLOEXEC, so a program that another thread starts between
  // these calls can inherit the sockets. base::LaunchProcess() passes only
  // what it is told to, and the Page program watches its parent for the rest.
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
    return false;
  }
  ours.reset(sockets[0]);
  theirs.reset(sockets[1]);
  const int on = 1;
  for (const int socket : sockets) {
    if (fcntl(socket, F_SETFD, FD_CLOEXEC) != 0 ||
        setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) != 0) {
      return false;
    }
  }
#else
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
    return false;
  }
  ours.reset(sockets[0]);
  theirs.reset(sockets[1]);
#endif
  return true;
}

PageChannelEnd PageChannelEndFromArgument(std::string_view argument) {
  int descriptor = -1;
  struct stat status;
  // Standard input, output and error are not the channel.
  if (!base::StringToInt(argument, &descriptor) || descriptor <= 2 ||
      fstat(descriptor, &status) != 0 || !S_ISSOCK(status.st_mode)) {
    return PageChannelEnd();
  }
  return PageChannelEnd(descriptor);
}

}  // namespace mitto
