#include <windows.h>

#include <stdint.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/containers/circular_deque.h"
#include "base/containers/span.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/message_loop/message_pump_for_io.h"
#include "base/numerics/safe_conversions.h"
#include "base/rand_util.h"
#include "base/strings/strcat.h"
#include "base/strings/strcat_win.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_number_conversions_win.h"
#include "base/task/current_thread.h"
#include "base/task/single_thread_task_runner.h"
#include "base/win/scoped_handle.h"
#include "mitto/page_channel.h"
#include "mitto/page_messages.h"

namespace mitto {
namespace {

// How much one read takes from the pipe at most.
constexpr size_t kReadSize = 64 * 1024;

class PageChannelWin;

// The pipe and the reads and writes on it, which the system completes on the
// thread's completion port. It lives until the last of them has completed,
// because until then the system may still write into its buffers, so it can
// outlive the PageChannelWin that owns it.
// Upstream: mojo/core/channel_win.cc
class PipeIo final : public base::RefCounted<PipeIo>,
                     public base::MessagePumpForIO::IOHandler {
 public:
  PipeIo(base::win::ScopedHandle pipe, PageChannelWin& owner)
      : base::MessagePumpForIO::IOHandler(FROM_HERE),
        pipe_(std::move(pipe)),
        owner_(&owner) {}

  bool Start() {
    if (!base::CurrentIOThread::Get()->RegisterIOHandler(pipe_.get(), this)) {
      return false;
    }
    ReadMore();
    return true;
  }

  void Send(std::string frame) {
    if (!writable_) {
      return;
    }
    outgoing_.push_back(std::move(frame));
    if (!write_pending_) {
      WriteNext();
    }
  }

  bool all_written() const { return outgoing_.empty() || !writable_; }

  // The owner has gone. Pending reads and writes end with
  // ERROR_OPERATION_ABORTED, and the owner hears of nothing more.
  void ShutDown() {
    owner_ = nullptr;
    writable_ = false;
    outgoing_.clear();
    // CancelIo() would only cancel what this thread started.
    CancelIoEx(pipe_.get(), nullptr);
    pipe_.Close();
  }

 private:
  friend class base::RefCounted<PipeIo>;

  ~PipeIo() override = default;

  // base::MessagePumpForIO::IOHandler:
  void OnIOCompleted(base::MessagePumpForIO::IOContext* context,
                     DWORD bytes_transferred,
                     DWORD error) override;

  void ReadMore();
  void WriteNext();

  base::win::ScopedHandle pipe_;
  // Null once the owner has gone.
  raw_ptr<PageChannelWin> owner_;
  base::MessagePumpForIO::IOContext read_context_;
  base::MessagePumpForIO::IOContext write_context_;
  std::vector<uint8_t> read_buffer_ = std::vector<uint8_t>(kReadSize);
  bool write_pending_ = false;
  bool writable_ = true;
  // The first is being written.
  base::circular_deque<std::string> outgoing_;
};

class PageChannelWin final : public PageChannel {
 public:
  PageChannelWin(base::win::ScopedHandle pipe, Delegate& delegate)
      : io_(base::MakeRefCounted<PipeIo>(std::move(pipe), *this)),
        delegate_(delegate) {}

  ~PageChannelWin() override { io_->ShutDown(); }

  bool Start() { return io_->Start(); }

  // PageChannel:
  void Send(std::string frame) override {
    if (readable_) {
      io_->Send(std::move(frame));
    }
  }

  void WhenWritten(base::OnceClosure done) override {
    written_ = std::move(done);
    OnWritten();
  }

  // From PipeIo.
  void OnBytesRead(base::span<const uint8_t> bytes) {
    reader_.Append(bytes);
    const base::WeakPtr<PageChannelWin> alive = weak_factory_.GetWeakPtr();
    uint32_t type = 0;
    std::string payload;
    size_t size = 0;
    while (true) {
      switch (reader_.Next(type, payload, size)) {
        case FrameReader::Result::kNeedMore:
          return;
        case FrameReader::Result::kTooLarge:
          frame_too_large_ = true;
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

  // Whether PipeIo reads on.
  bool wants_more() const { return readable_ && !frame_too_large_; }

  void OnClosed(Closed closed, std::string why) {
    if (!readable_ || frame_too_large_) {
      return;
    }
    readable_ = false;
    OnWritten();
    delegate_->OnChannelClosed(closed, std::move(why));
  }

  void OnWritten() {
    if (written_ && (!readable_ || io_->all_written())) {
      // From a task of its own, because it may destroy the channel.
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, std::move(written_));
    }
  }

 private:
  const scoped_refptr<PipeIo> io_;
  const raw_ref<Delegate> delegate_;
  bool readable_ = true;
  bool frame_too_large_ = false;
  FrameReader reader_;
  base::OnceClosure written_;
  base::WeakPtrFactory<PageChannelWin> weak_factory_{this};
};

void PipeIo::OnIOCompleted(base::MessagePumpForIO::IOContext* context,
                           DWORD bytes_transferred,
                           DWORD error) {
  // Drops the reference that the read or write took when it started, but only
  // once this returns.
  const scoped_refptr<PipeIo> self(this);
  Release();
  if (context == &write_context_) {
    write_pending_ = false;
    // ShutDown() has emptied it.
    if (outgoing_.empty()) {
      return;
    }
    if (error != ERROR_SUCCESS ||
        bytes_transferred != outgoing_.front().size()) {
      // The other end has gone. What it sent before still waits to be read,
      // and the broken pipe that follows says that it has gone.
      writable_ = false;
      outgoing_.clear();
    } else {
      outgoing_.pop_front();
      if (!outgoing_.empty()) {
        WriteNext();
      }
    }
    if (owner_) {
      owner_->OnWritten();
    }
    return;
  }

  if (!owner_) {
    return;
  }
  if (error == ERROR_SUCCESS && bytes_transferred > 0) {
    owner_->OnBytesRead(base::span(read_buffer_).first(bytes_transferred));
    if (owner_ && owner_->wants_more()) {
      ReadMore();
    }
    return;
  }
  if (error == ERROR_SUCCESS || error == ERROR_BROKEN_PIPE ||
      error == ERROR_PIPE_NOT_CONNECTED) {
    owner_->OnClosed(PageChannel::Closed::kByOtherEnd,
                     "the other end closed the channel");
    return;
  }
  owner_->OnClosed(PageChannel::Closed::kFailed,
                   base::StrCat({"reading the channel failed: error ",
                                 base::NumberToString(error)}));
}

void PipeIo::ReadMore() {
  if (ReadFile(pipe_.get(), read_buffer_.data(),
               base::checked_cast<DWORD>(read_buffer_.size()), nullptr,
               read_context_.GetOverlapped()) ||
      GetLastError() == ERROR_IO_PENDING) {
    // The completion comes through the completion port either way.
    AddRef();
    return;
  }
  const DWORD error = GetLastError();
  if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) {
    owner_->OnClosed(PageChannel::Closed::kByOtherEnd,
                     "the other end closed the channel");
    return;
  }
  owner_->OnClosed(PageChannel::Closed::kFailed,
                   base::StrCat({"reading the channel failed: error ",
                                 base::NumberToString(error)}));
}

void PipeIo::WriteNext() {
  const std::string& frame = outgoing_.front();
  if (WriteFile(pipe_.get(), frame.data(),
                base::checked_cast<DWORD>(frame.size()), nullptr,
                write_context_.GetOverlapped()) ||
      GetLastError() == ERROR_IO_PENDING) {
    write_pending_ = true;
    AddRef();
    return;
  }
  writable_ = false;
  outgoing_.clear();
}

// A new, unique name for a pipe. "LOCAL" keeps it to this session's processes,
// and allows a sandbox to hold the Page later.
// Upstream: mojo/public/cpp/platform/named_platform_channel_win.cc
std::wstring NewPipeName() {
  return base::StrCat({L"\\\\.\\pipe\\LOCAL\\mitto.",
                       base::NumberToWString(GetCurrentProcessId()), L".",
                       base::NumberToWString(GetCurrentThreadId()), L".",
                       base::NumberToWString(base::RandUint64())});
}

}  // namespace

// static
std::unique_ptr<PageChannel> PageChannel::Start(PageChannelEnd end,
                                                Delegate& delegate) {
  if (!end.is_valid()) {
    return nullptr;
  }
  auto channel = std::make_unique<PageChannelWin>(std::move(end), delegate);
  if (!channel->Start()) {
    return nullptr;
  }
  return channel;
}

// Upstream: mojo/public/cpp/platform/platform_channel.cc
bool CreatePageChannel(PageChannelEnd& ours, PageChannelEnd& theirs) {
  constexpr DWORD kOpenMode =
      PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE;
  constexpr DWORD kPipeMode =
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS;
  // FILE_APPEND_DATA is FILE_CREATE_PIPE_INSTANCE on a pipe, which this end
  // never needs, and GENERIC_WRITE would include it.
  constexpr DWORD kDesiredAccess =
      (FILE_GENERIC_READ | FILE_GENERIC_WRITE) & ~FILE_APPEND_DATA;
  // SECURITY_ANONYMOUS keeps this end from acting as the Page's user.
  constexpr DWORD kFlags =
      SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS | FILE_FLAG_OVERLAPPED;
  // A process that connected to the pipe between these two calls has it, and
  // ours fails with ERROR_PIPE_BUSY. Then both go, and a new name is tried.
  for (int attempt = 0; attempt < 2; ++attempt) {
    const std::wstring name = NewPipeName();
    // Only this process holds its end: a handle is not inherited by default.
    ours.Set(CreateNamedPipeW(name.c_str(), kOpenMode, kPipeMode,
                              /*nMaxInstances=*/1, /*nOutBufferSize=*/4096,
                              /*nInBufferSize=*/4096, /*nDefaultTimeOut=*/5000,
                              /*lpSecurityAttributes=*/nullptr));
    if (!ours.is_valid()) {
      return false;
    }
    SECURITY_ATTRIBUTES inherited = {sizeof(SECURITY_ATTRIBUTES), nullptr,
                                     TRUE};
    theirs.Set(CreateFileW(name.c_str(), kDesiredAccess, 0, &inherited,
                           OPEN_EXISTING, kFlags, nullptr));
    if (theirs.is_valid()) {
      // Connected already, so this says so rather than waiting.
      return !ConnectNamedPipe(ours.get(), nullptr) &&
             GetLastError() == ERROR_PIPE_CONNECTED;
    }
    if (GetLastError() != ERROR_PIPE_BUSY) {
      return false;
    }
  }
  return false;
}

PageChannelEnd PageChannelEndFromArgument(std::string_view argument) {
  uint32_t value = 0;
  if (!base::StringToUint(argument, &value) || value == 0) {
    return PageChannelEnd();
  }
  // A handle's value fits in 32 bits, even in a 64-bit process, so that 32-bit
  // and 64-bit processes can pass handles.
  const HANDLE handle = ULongToHandle(value);
  if (GetFileType(handle) != FILE_TYPE_PIPE) {
    return PageChannelEnd();
  }
  return PageChannelEnd(handle);
}

}  // namespace mitto
