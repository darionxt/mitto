// The messages between a Page and its Session, and the frames that carry them
// over the channel between their two processes. The library and the Page
// program both build this file. The Session reads what a Page sends as hostile
// input, so each frame from a Page is checked here, field by field, before the
// library acts on it.
//
// docs/runtime.md describes the channel and each message.

#ifndef MITTO_PAGE_MESSAGES_H_
#define MITTO_PAGE_MESSAGES_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <string_view>

#include "base/containers/span.h"
#include "base/types/expected.h"

namespace mitto {

// Both ends speak exactly this version and refuse any other, because the Page
// program ships beside the library, built from the same tree. Increase it with
// any change to the messages below.
inline constexpr uint32_t kPageProtocolVersion = 1;

// A frame is the size of its payload and its type, each a 32-bit little-endian
// number, and then the payload.
inline constexpr size_t kFrameHeaderSize = 8;

// Each end refuses a frame with a larger payload, so that the other cannot make
// it hold more than this for one message.
inline constexpr size_t kMaxPayloadSize = 16 * 1024 * 1024;

// The longest script that one kEvaluate carries.
inline constexpr size_t kMaxScriptSize = kMaxPayloadSize - 8;

// The longest result or reason that one kEvaluated carries.
inline constexpr size_t kMaxEvaluatedTextSize = kMaxPayloadSize - 9;

// The argument that starts the Page program for a Session:
// "--session=CHANNEL,PARENT", where CHANNEL is the descriptor of its end of the
// channel on macOS and Linux and the value of its handle on Windows, and PARENT
// is the process ID of the process that holds the library.
inline constexpr std::string_view kSessionArgumentPrefix = "--session=";

// How the Page program ends, beside a crash or a kill. 0 says that the channel
// closed.
//
// The Page refused a message, or could not start.
inline constexpr int kPageExitRefused = 1;
// V8 ran out of memory.
inline constexpr int kPageExitOutOfMemory = 3;

// The types of message. Each says which way it goes and what its payload holds.
enum class PageMessage : uint32_t {
  // Each way, first and only once: the version of these messages that the
  // sender speaks, as a 32-bit number.
  kHello = 1,
  // Each way: why the sender refuses what it got, in UTF-8. The sender closes
  // the channel after it.
  kRefused = 2,
  // To the Page, once, right after kHello: what the Page starts with, as the
  // JSON object that EncodeStart() writes.
  kStart = 3,
  // To the Page: the number of a call, as a 64-bit number, and then a script,
  // in UTF-8.
  kEvaluate = 4,
  // To the Session: the number of the call, a byte that is 0 if the script has
  // a result and 1 if it failed, and then the result as JSON, or why it
  // failed, in UTF-8.
  kEvaluated = 5,
};

// What a Page starts with, which it would otherwise read from the machine.
struct PageStart {
  PageStart();
  PageStart(const PageStart&);
  PageStart& operator=(const PageStart&);
  ~PageStart();

  // The IANA name of the time zone that ICU, and so V8, reports.
  std::string time_zone;
  // The BCP 47 tag of ICU's default locale, which Intl falls back to.
  std::string locale;
};

// What a call to a Page gave back.
struct PageEvaluated {
  PageEvaluated();
  PageEvaluated(PageEvaluated&&);
  PageEvaluated& operator=(PageEvaluated&&);
  ~PageEvaluated();

  uint64_t call = 0;
  // Whether the script failed, and `text` says why. Otherwise `text` is the
  // result as JSON, empty if JSON.stringify() writes nothing for it.
  bool failed = false;
  std::string text;
};

// A frame from a Page, once checked. Which fields it uses depends on `type`.
struct FrameFromPage {
  FrameFromPage();
  FrameFromPage(FrameFromPage&&);
  FrameFromPage& operator=(FrameFromPage&&);
  ~FrameFromPage();

  PageMessage type = PageMessage::kHello;
  // kHello.
  uint32_t version = 0;
  // kRefused: why the Page refused.
  std::string reason;
  // kEvaluated.
  PageEvaluated evaluated;
};

// A frame from a Session, once checked. Which fields it uses depends on
// `type`.
struct FrameFromSession {
  FrameFromSession();
  FrameFromSession(FrameFromSession&&);
  FrameFromSession& operator=(FrameFromSession&&);
  ~FrameFromSession();

  PageMessage type = PageMessage::kHello;
  // kHello.
  uint32_t version = 0;
  // kRefused: why the Session refused.
  std::string reason;
  // kStart.
  PageStart start;
  // kEvaluate.
  uint64_t call = 0;
  std::string script;
};

// Returns the frame of a message of `type` with `payload`, which is at most
// kMaxPayloadSize bytes.
std::string EncodeFrame(PageMessage type, std::string_view payload);

std::string EncodeHello();
std::string EncodeStart(const PageStart& start);
// `script` is at most kMaxScriptSize bytes.
std::string EncodeEvaluate(uint64_t call, std::string_view script);
// `evaluated.text` is at most kMaxEvaluatedTextSize bytes.
std::string EncodeEvaluated(const PageEvaluated& evaluated);

// Returns what a frame from a Page says, or why the Session refuses it.
base::expected<FrameFromPage, std::string> ReadFrameFromPage(
    uint32_t type,
    std::string payload);

// Returns what a frame from a Session says, or why the Page refuses it.
base::expected<FrameFromSession, std::string> ReadFrameFromSession(
    uint32_t type,
    std::string payload);

// Cuts the bytes that arrive into frames.
class FrameReader {
 public:
  enum class Result {
    kFrame,
    kNeedMore,
    // The next frame's payload is larger than kMaxPayloadSize, and nothing more
    // can be read.
    kTooLarge,
  };

  FrameReader();

  FrameReader(const FrameReader&) = delete;
  FrameReader& operator=(const FrameReader&) = delete;

  ~FrameReader();

  void Append(base::span<const uint8_t> bytes);

  // Takes the next whole frame into `type` and `payload`, if one has arrived.
  // After kTooLarge, `size` holds the size that the frame claimed.
  Result Next(uint32_t& type, std::string& payload, size_t& size);

 private:
  std::string buffer_;
  // Where the next frame starts in `buffer_`.
  size_t offset_ = 0;
};

}  // namespace mitto

#endif  // MITTO_PAGE_MESSAGES_H_
