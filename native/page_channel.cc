#include "mitto/page_channel.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <utility>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/location.h"
#include "base/numerics/byte_conversions.h"
#include "base/posix/eintr_wrapper.h"
#include "base/posix/unix_domain_socket.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_view_util.h"
#include "base/task/current_thread.h"
#include "build/build_config.h"

namespace mitto {

namespace {

// Writes to a socket whose reader has gone must fail and not raise SIGPIPE,
// which would end the process that loaded the library.
#if BUILDFLAG(IS_APPLE)
constexpr int kSendFlags = 0;
#else
constexpr int kSendFlags = MSG_NOSIGNAL;
#endif

}  // namespace

bool IsKnownPageMessage(uint32_t type) {
  return type >= static_cast<uint32_t>(PageMessage::kHello) &&
         type <= static_cast<uint32_t>(PageMessage::kRefused);
}

std::string EncodeUint32(uint32_t value) {
  const std::array<uint8_t, 4> bytes = base::U32ToLittleEndian(value);
  return std::string(bytes.begin(), bytes.end());
}

std::string EncodeUint64(uint64_t value) {
  const std::array<uint8_t, 8> bytes = base::U64ToLittleEndian(value);
  return std::string(bytes.begin(), bytes.end());
}

bool DecodeUint32(std::string_view bytes, uint32_t& value) {
  if (bytes.size() != 4) {
    return false;
  }
  value = base::U32FromLittleEndian(base::as_byte_span(bytes).first<4>());
  return true;
}

bool DecodeUint64(std::string_view bytes, uint64_t& value) {
  if (bytes.size() != 8) {
    return false;
  }
  value = base::U64FromLittleEndian(base::as_byte_span(bytes).first<8>());
  return true;
}

std::string EncodeFrame(uint32_t type, std::string_view payload) {
  return base::StrCat(
      {EncodeUint32(static_cast<uint32_t>(payload.size())), EncodeUint32(type),
       payload});
}

FrameReader::FrameReader() = default;
FrameReader::~FrameReader() = default;

void FrameReader::Append(base::span<const uint8_t> bytes) {
  buffer_.append(base::as_string_view(bytes));
}

FrameReader::Result FrameReader::Next(uint32_t& type, std::string& payload) {
  const std::string_view rest = std::string_view(buffer_).substr(offset_);
  if (rest.size() < kFrameHeaderSize) {
    return Result::kNeedMore;
  }
  uint32_t size = 0;
  DecodeUint32(rest.substr(0, 4), size);
  DecodeUint32(rest.substr(4, 4), type);
  if (size > kMaxPayloadSize) {
    return Result::kTooLarge;
  }
  if (rest.size() < kFrameHeaderSize + size) {
    return Result::kNeedMore;
  }
  payload.assign(rest.substr(kFrameHeaderSize, size));
  offset_ += kFrameHeaderSize + size;
  if (offset_ == buffer_.size()) {
    buffer_.clear();
    offset_ = 0;
  }
  return Result::kFrame;
}

ChannelEnd::ChannelEnd(base::ScopedFD socket, Delegate* delegate)
    : socket_(std::move(socket)),
      delegate_(delegate),
      read_controller_(FROM_HERE),
      write_controller_(FROM_HERE) {}

ChannelEnd::~ChannelEnd() = default;

bool ChannelEnd::Start() {
  return base::CurrentIOThread::Get()->WatchFileDescriptor(
      socket_.get(), /*persistent=*/true, base::MessagePumpForIO::WATCH_READ,
      &read_controller_, this);
}

void ChannelEnd::Send(PageMessage type, std::string_view payload) {
  Write(EncodeFrame(type, payload));
}

void ChannelEnd::SendUnknownForTest(uint32_t type) {
  Write(EncodeFrame(type, ""));
}

void ChannelEnd::OnFileCanReadWithoutBlocking(int fd) {
  std::array<uint8_t, 64 * 1024> buffer;
  const ssize_t read =
      HANDLE_EINTR(recv(socket_.get(), buffer.data(), buffer.size(), 0));
  if (read == 0) {
    Close("the other end closed the channel");
    return;
  }
  if (read < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }
    Close(base::StrCat({"reading failed: ", base::NumberToString(errno)}));
    return;
  }
  reader_.Append(base::span(buffer).first(static_cast<size_t>(read)));
  uint32_t type = 0;
  std::string payload;
  while (true) {
    const FrameReader::Result result = reader_.Next(type, payload);
    if (result == FrameReader::Result::kNeedMore) {
      return;
    }
    if (result == FrameReader::Result::kTooLarge) {
      Close("a frame was larger than the channel allows");
      return;
    }
    delegate_->OnFrame(type, std::move(payload));
    // The delegate may have closed the channel.
    if (!socket_.is_valid()) {
      return;
    }
  }
}

void ChannelEnd::OnFileCanWriteWithoutBlocking(int fd) {
  Flush();
}

void ChannelEnd::Write(std::string frame) {
  if (!socket_.is_valid()) {
    return;
  }
  unwritten_.append(frame);
  Flush();
}

void ChannelEnd::Flush() {
  while (!unwritten_.empty()) {
    const ssize_t written = HANDLE_EINTR(
        send(socket_.get(), unwritten_.data(), unwritten_.size(), kSendFlags));
    if (written < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (!watching_write_) {
          watching_write_ = base::CurrentIOThread::Get()->WatchFileDescriptor(
              socket_.get(), /*persistent=*/true,
              base::MessagePumpForIO::WATCH_WRITE, &write_controller_, this);
        }
        return;
      }
      Close(base::StrCat({"writing failed: ", base::NumberToString(errno)}));
      return;
    }
    unwritten_.erase(0, static_cast<size_t>(written));
  }
  if (watching_write_) {
    write_controller_.StopWatchingFileDescriptor();
    watching_write_ = false;
  }
}

void ChannelEnd::Shutdown() {
  read_controller_.StopWatchingFileDescriptor();
  write_controller_.StopWatchingFileDescriptor();
  watching_write_ = false;
  socket_.reset();
  unwritten_.clear();
}

void ChannelEnd::Close(std::string_view why) {
  if (!socket_.is_valid()) {
    return;
  }
  Shutdown();
  delegate_->OnChannelClosed(why);
}

bool CreateChannelSockets(base::ScopedFD& ours, base::ScopedFD& theirs) {
  int sockets[2];
#if BUILDFLAG(IS_APPLE)
  // macOS has no SOCK_CLOEXEC, so another thread that starts a program between
  // these two calls can leak the sockets into it. The Page program also
  // watches its parent for that reason.
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
    return false;
  }
  ours.reset(sockets[0]);
  theirs.reset(sockets[1]);
  if (fcntl(ours.get(), F_SETFD, FD_CLOEXEC) != 0 ||
      fcntl(theirs.get(), F_SETFD, FD_CLOEXEC) != 0) {
    return false;
  }
  const int on = 1;
  if (setsockopt(ours.get(), SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) != 0) {
    return false;
  }
#else
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
    return false;
  }
  ours.reset(sockets[0]);
  theirs.reset(sockets[1]);
#endif
  return base::SetNonBlocking(ours.get());
}

}  // namespace mitto
