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
inline constexpr uint32_t kPageProtocolVersion = 3;

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

// The longest text that one kConsole carries.
inline constexpr size_t kMaxConsoleTextSize = kMaxPayloadSize - 1;

// The number of the request that the Session makes when the Page opens: its
// navigation, whose response is the Page's document. The Page numbers its own
// requests from 1 up.
inline constexpr uint64_t kNavigationRequest = 0;

// The net error of a kResponseEnded whose request has all of its body:
// net::OK, which the Page program, with no //net, cannot name.
inline constexpr int32_t kResponseComplete = 0;

// How many bytes of a response's body the Session sends before the Page gives
// credit for them, as much as RequestChannel holds for a caller. Each request
// starts with this much.
inline constexpr uint32_t kBodyWindow = 64 * 1024;

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
  // To the Session: a byte that holds a ConsoleLevel, and then the text of a
  // message that a script wrote to the console, in UTF-8.
  kConsole = 6,
  // To the Session: an error that no script caught, such as a promise rejected
  // with no handler, in UTF-8.
  kUncaughtError = 7,
  // To the Session: a request of the Page's, as PageRequest says. Its number
  // is larger than that of each request before it.
  kRequest = 8,
  // To the Session: the number of a request, as a 64-bit number, and how many
  // more bytes of its body the Page has taken, as a 32-bit number. The Session
  // may send that many more.
  kCredit = 9,
  // To the Session: the number of a question for the document's cookies, as a
  // 64-bit number. kCookies answers it.
  kGetCookies = 10,
  // To the Session: what a script set document.cookie to, in UTF-8.
  kSetCookie = 11,
  // To the Session, with no payload: the window's load event has been
  // dispatched.
  kLoaded = 12,
  // To the Page: the response of a request, as PageResponseStarted says.
  kResponseStarted = 13,
  // To the Page: the number of a request, as a 64-bit number, and the next
  // bytes of its body, at most as many as the Page has given credit for.
  kResponseBody = 14,
  // To the Page: the number of a request, as a 64-bit number, and a net error,
  // as a 32-bit number: 0 when all of the body has come, and otherwise why the
  // request failed, with or without a response.
  kResponseEnded = 15,
  // To the Page: the number of a kGetCookies, as a 64-bit number, and the
  // document's cookies as a Cookie header's line holds them.
  kCookies = 16,
};

// The Request Kinds that a Page asks its Session for, beside the navigation
// that opens it.
enum class PageRequestKind : uint8_t {
  kScript = 1,
  kFetch = 2,
};

// The level of a console message, as the console method that wrote it says.
enum class ConsoleLevel : uint8_t {
  kDebug = 1,
  kLog = 2,
  kInfo = 3,
  kWarning = 4,
  kError = 5,
};

// A message that a script wrote to the console.
struct PageConsoleMessage {
  PageConsoleMessage();
  PageConsoleMessage(PageConsoleMessage&&);
  PageConsoleMessage& operator=(PageConsoleMessage&&);
  ~PageConsoleMessage();

  ConsoleLevel level = ConsoleLevel::kLog;
  std::string text;
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

// A request that a Page asks its Session for. Its Initiator is the Page's
// document, as the Session loaded it, so the Page does not say it.
struct PageRequest {
  PageRequest();
  PageRequest(PageRequest&&);
  PageRequest& operator=(PageRequest&&);
  ~PageRequest();

  uint64_t number = 0;
  PageRequestKind kind = PageRequestKind::kScript;
  // In UTF-8, as the Page resolved it. The Session checks that it is a URL.
  std::string url;
};

// How much more of a request's body a Page has taken.
struct PageCredit {
  uint64_t number = 0;
  uint32_t bytes = 0;
};

// The response of a request, after any redirects, which the Session follows
// as a request of its own does.
struct PageResponseStarted {
  PageResponseStarted();
  PageResponseStarted(PageResponseStarted&&);
  PageResponseStarted& operator=(PageResponseStarted&&);
  ~PageResponseStarted();

  uint64_t number = 0;
  int32_t status = 0;
  // The URL of the response, in UTF-8.
  std::string url;
  // The header lines, each "name: value" and a line feed, as the C API's
  // events hold them, but for Set-Cookie and Set-Cookie2.
  std::string headers;
};

// Bytes of a response's body.
struct PageResponseBody {
  PageResponseBody();
  PageResponseBody(PageResponseBody&&);
  PageResponseBody& operator=(PageResponseBody&&);
  ~PageResponseBody();

  uint64_t number = 0;
  std::string bytes;
};

// The end of a request.
struct PageResponseEnded {
  uint64_t number = 0;
  // net::OK when all of the body has come.
  int32_t net_error = 0;
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
  // kConsole.
  PageConsoleMessage console;
  // kUncaughtError.
  std::string uncaught_error;
  // kRequest.
  PageRequest request;
  // kCredit.
  PageCredit credit;
  // kGetCookies.
  uint64_t cookies_question = 0;
  // kSetCookie.
  std::string cookie_line;
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
  // kResponseStarted.
  PageResponseStarted response;
  // kResponseBody.
  PageResponseBody body;
  // kResponseEnded.
  PageResponseEnded ended;
  // kCookies: the number of the question, and the line.
  uint64_t cookies_answer = 0;
  std::string cookies;
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
// `message.text` is at most kMaxConsoleTextSize bytes.
std::string EncodeConsole(const PageConsoleMessage& message);
// `text` is at most kMaxPayloadSize bytes.
std::string EncodeUncaughtError(std::string_view text);
// `request.url` is at most kMaxPayloadSize - 9 bytes.
std::string EncodeRequest(const PageRequest& request);
std::string EncodeCredit(const PageCredit& credit);
std::string EncodeGetCookies(uint64_t question);
// `line` is at most kMaxPayloadSize bytes.
std::string EncodeSetCookie(std::string_view line);
std::string EncodeLoaded();
// The URL and the headers fit in a frame.
std::string EncodeResponseStarted(const PageResponseStarted& response);
// `bytes` is at most kBodyWindow bytes.
std::string EncodeResponseBody(uint64_t number, std::string_view bytes);
std::string EncodeResponseEnded(const PageResponseEnded& ended);
// `line` is at most kMaxPayloadSize - 8 bytes.
std::string EncodeCookies(uint64_t answer, std::string_view line);

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
