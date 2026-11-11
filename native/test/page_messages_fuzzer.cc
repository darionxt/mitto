// Feeds any bytes to the code that reads what a Page sends its Session over
// their channel: the frame reader and the checks of each message. They come
// from the Page process, which runs a site's scripts and may be hostile.
//
// The first byte of the input says how the channel cuts up the rest, which is
// the frames.

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <string>

#include "base/check.h"
#include "base/check_op.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/notreached.h"
#include "base/strings/string_util.h"
#include "base/types/expected.h"
#include "mitto/page_messages.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // SAFETY: libFuzzer says `data` holds `size` bytes.
  base::span<const uint8_t> input = UNSAFE_BUFFERS(base::span(data, size));
  if (input.empty()) {
    return 0;
  }
  const size_t piece = std::max<size_t>(input.front(), 1);
  input = input.subspan(1u);

  mitto::FrameReader reader;
  while (!input.empty()) {
    const size_t taken = std::min(piece, input.size());
    reader.Append(input.first(taken));
    input = input.subspan(taken);
    uint32_t type = 0;
    std::string payload;
    size_t frame_size = 0;
    while (true) {
      const mitto::FrameReader::Result result =
          reader.Next(type, payload, frame_size);
      if (result == mitto::FrameReader::Result::kNeedMore) {
        break;
      }
      if (result == mitto::FrameReader::Result::kTooLarge) {
        CHECK_GT(frame_size, mitto::kMaxPayloadSize);
        return 0;
      }
      CHECK_LE(payload.size(), mitto::kMaxPayloadSize);
      const base::expected<mitto::FrameFromPage, std::string> frame =
          mitto::ReadFrameFromPage(type, payload);
      if (!frame.has_value()) {
        CHECK(!frame.error().empty());
        continue;
      }
      switch (frame->type) {
        case mitto::PageMessage::kHello:
          CHECK_EQ(payload.size(), 4u);
          break;
        case mitto::PageMessage::kRefused:
          CHECK(base::IsStringUTF8AllowingNoncharacters(frame->reason));
          break;
        case mitto::PageMessage::kEvaluated:
          CHECK(base::IsStringUTF8AllowingNoncharacters(frame->evaluated.text));
          CHECK_EQ(frame->evaluated.text.size() + 9, payload.size());
          break;
        case mitto::PageMessage::kConsole:
          CHECK(base::IsStringUTF8AllowingNoncharacters(frame->console.text));
          CHECK_EQ(frame->console.text.size() + 1, payload.size());
          break;
        case mitto::PageMessage::kUncaughtError:
          CHECK(base::IsStringUTF8AllowingNoncharacters(frame->uncaught_error));
          break;
        case mitto::PageMessage::kRequest:
          CHECK_NE(frame->request.number, mitto::kNavigationRequest);
          CHECK(base::IsStringUTF8AllowingNoncharacters(frame->request.url));
          CHECK_EQ(frame->request.url.size() + 9, payload.size());
          break;
        case mitto::PageMessage::kCredit:
          CHECK_GT(frame->credit.bytes, 0u);
          CHECK_LE(frame->credit.bytes, mitto::kBodyWindow);
          break;
        case mitto::PageMessage::kGetCookies:
          CHECK_EQ(payload.size(), 8u);
          break;
        case mitto::PageMessage::kSetCookie:
          CHECK(base::IsStringUTF8AllowingNoncharacters(frame->cookie_line));
          break;
        case mitto::PageMessage::kLoaded:
          CHECK(payload.empty());
          break;
        case mitto::PageMessage::kStart:
        case mitto::PageMessage::kEvaluate:
        case mitto::PageMessage::kResponseStarted:
        case mitto::PageMessage::kResponseBody:
        case mitto::PageMessage::kResponseEnded:
        case mitto::PageMessage::kCookies:
          // Only a Session sends these.
          NOTREACHED();
      }
    }
  }
  return 0;
}
