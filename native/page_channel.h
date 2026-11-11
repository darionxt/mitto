// One end of the channel between a Page and its Session: a socket of a socket
// pair on macOS and Linux, and a named pipe on Windows. It carries the frames of
// page_messages.h, and both programs build it.

#ifndef MITTO_PAGE_CHANNEL_H_
#define MITTO_PAGE_CHANNEL_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "build/build_config.h"
#include "mitto/page_messages.h"

#if BUILDFLAG(IS_WIN)
#include "base/win/scoped_handle.h"
#else
#include "base/files/scoped_file.h"
#endif

namespace mitto {

// The system's handle of one end of the channel.
#if BUILDFLAG(IS_WIN)
using PageChannelEnd = base::win::ScopedHandle;
#else
using PageChannelEnd = base::ScopedFD;
#endif

// One end of the channel, on the thread that starts it, which runs an I/O
// message pump. It hands each whole frame to its delegate, and keeps what the
// system cannot take yet, so Send() never blocks.
class PageChannel {
 public:
  // Why the channel carries no more.
  enum class Closed {
    // The other end closed the channel, or its process ended.
    kByOtherEnd,
    // Reading or writing failed.
    kFailed,
    // The other end sent a frame larger than kMaxPayloadSize. Nothing more is
    // read, but Send() still writes, so that the delegate can refuse it.
    kFrameTooLarge,
  };

  class Delegate {
   public:
    // May destroy the channel.
    virtual void OnFrame(uint32_t type, std::string payload) = 0;
    // Nothing reaches the delegate after this. May destroy the channel.
    virtual void OnChannelClosed(Closed closed, std::string why) = 0;

   protected:
    virtual ~Delegate() = default;
  };

  // Starts to read `end` on the current thread. Returns null if it cannot.
  static std::unique_ptr<PageChannel> Start(PageChannelEnd end,
                                            Delegate& delegate);

  PageChannel(const PageChannel&) = delete;
  PageChannel& operator=(const PageChannel&) = delete;

  // Closes the channel without telling the delegate. What the system has taken
  // already still reaches the other end, which then reads the end of the
  // stream.
  virtual ~PageChannel() = default;

  // Sends a frame that EncodeFrame() or one of the functions that call it has
  // made. Does nothing once the channel is closed.
  virtual void Send(std::string frame) = 0;

  // Runs `done` once what was sent so far has been written, or the channel can
  // write no more. Only the last `done` given runs.
  virtual void WhenWritten(base::OnceClosure done) = 0;

 protected:
  PageChannel() = default;
};

// Makes both ends of a new channel: `ours` for this process, which only this
// process holds, and `theirs` for the Page process, which inherits it. Returns
// false if the system has no room for them.
bool CreatePageChannel(PageChannelEnd& ours, PageChannelEnd& theirs);

// Returns the end that the Page program inherited, which `argument` names, or
// an invalid one if it names none.
PageChannelEnd PageChannelEndFromArgument(std::string_view argument);

}  // namespace mitto

#endif  // MITTO_PAGE_CHANNEL_H_
