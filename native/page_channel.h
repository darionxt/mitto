// A prototype of the channel between a Page and its Session, on //base alone.
// Nothing of it merges: it answers which channel the Runtime takes.

#ifndef MITTO_PAGE_CHANNEL_H_
#define MITTO_PAGE_CHANNEL_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <string_view>

#include "base/containers/span.h"
#include "base/files/scoped_file.h"
#include "base/memory/raw_ptr.h"
#include "base/message_loop/message_pump_for_io.h"

namespace mitto {

// Both ends speak this version and refuse any other: the Page program ships
// beside the library, built from the same tree.
inline constexpr uint32_t kPageProtocolVersion = 1;

// The Page program finds its end of the channel at this descriptor.
inline constexpr int kPageChannelDescriptor = 3;

// A frame is its payload's size and its type, as two 32-bit little-endian
// numbers, and then the payload. Either end refuses a bigger one, so that a
// Page cannot make its Session hold more than this for one message.
inline constexpr size_t kFrameHeaderSize = 8;
inline constexpr size_t kMaxPayloadSize = 1024 * 1024;

// How much of a body may be on its way to the Page and not yet read by it. The
// Session sends no more until the Page gives credit back, so a Page that reads
// slowly slows down the Session's reads from the network.
inline constexpr uint32_t kBodyWindow = 64 * 1024;
inline constexpr size_t kBodyChunkSize = 16 * 1024;

enum class PageMessage : uint32_t {
  // Each way, first: kPageProtocolVersion as 4 bytes.
  kHello = 1,
  // To the Page: a script. To the Session: its result.
  kEvaluate = 2,
  kResult = 3,
  // To the Page: a body of the size in the 8 bytes, its bytes, and its end.
  kBodyStart = 4,
  kBodyData = 5,
  kBodyEnd = 6,
  // To the Session: how many more bytes of the body it may send, as 4 bytes.
  kBodyCredit = 7,
  // To the Session: what the Page saw of the body, as text.
  kBodyDone = 8,
  // Each way: why the sender is closing the channel, as text.
  kRefused = 9,
};

bool IsKnownPageMessage(uint32_t type);

std::string EncodeFrame(uint32_t type, std::string_view payload);
inline std::string EncodeFrame(PageMessage type, std::string_view payload) {
  return EncodeFrame(static_cast<uint32_t>(type), payload);
}

std::string EncodeUint32(uint32_t value);
std::string EncodeUint64(uint64_t value);
bool DecodeUint32(std::string_view bytes, uint32_t& value);
bool DecodeUint64(std::string_view bytes, uint64_t& value);

// Cuts the bytes that arrive into frames.
class FrameReader {
 public:
  enum class Result { kFrame, kNeedMore, kTooLarge };

  FrameReader();
  ~FrameReader();

  void Append(base::span<const uint8_t> bytes);

  // Takes the next whole frame, if one has arrived.
  Result Next(uint32_t& type, std::string& payload);

 private:
  std::string buffer_;
  size_t offset_ = 0;
};

// One end of the channel: a non-blocking stream socket, watched on the current
// thread, which has to run an IO message pump. It hands each frame to its
// delegate and keeps what the socket cannot take yet.
class ChannelEnd final : public base::MessagePumpForIO::FdWatcher {
 public:
  class Delegate {
   public:
    virtual void OnFrame(uint32_t type, std::string payload) = 0;
    // The other end has closed the channel or gone, or a frame was too large.
    // Nothing else reaches the delegate after this.
    virtual void OnChannelClosed(std::string_view why) = 0;

   protected:
    virtual ~Delegate() = default;
  };

  ChannelEnd(base::ScopedFD socket, Delegate* delegate);

  ChannelEnd(const ChannelEnd&) = delete;
  ChannelEnd& operator=(const ChannelEnd&) = delete;

  ~ChannelEnd() override;

  bool Start();
  void Send(PageMessage type, std::string_view payload);
  void SendUnknownForTest(uint32_t type);
  // Closes the channel without telling the delegate. What was sent already
  // still reaches the other end, which then reads the end of the stream.
  void Shutdown();
  bool is_open() const { return socket_.is_valid(); }

 private:
  // base::MessagePumpForIO::FdWatcher:
  void OnFileCanReadWithoutBlocking(int fd) override;
  void OnFileCanWriteWithoutBlocking(int fd) override;

  void Write(std::string frame);
  void Flush();
  void Close(std::string_view why);

  base::ScopedFD socket_;
  raw_ptr<Delegate> delegate_;
  base::MessagePumpForIO::FdWatchController read_controller_;
  base::MessagePumpForIO::FdWatchController write_controller_;
  bool watching_write_ = false;
  FrameReader reader_;
  std::string unwritten_;
};

// Makes the socket pair: the first end for this process, non-blocking, and the
// second for the child, which inherits it. Neither leaks into another child.
bool CreateChannelSockets(base::ScopedFD& ours, base::ScopedFD& theirs);

}  // namespace mitto

#endif  // MITTO_PAGE_CHANNEL_H_
