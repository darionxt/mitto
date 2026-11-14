// Feeds any bytes to the code that reads what a Page sends its Session over
// their channel: the frame reader and the checks of each message. They come
// from the Page process, which runs a site's scripts and may be hostile.
//
// The first byte of the input says how the channel cuts up the rest, which is
// the frames.

#include "mitto/page_messages.h"

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
#include "mitto/header_rules.h"

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
      // A message of a document names it first, never as 0.
      if (mitto::NamesDocument(frame->type)) {
        CHECK_GE(payload.size(), 8u);
        CHECK_NE(frame->document, mitto::kNoDocument);
      } else {
        CHECK_EQ(frame->document, mitto::kNoDocument);
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
          CHECK(mitto::IsHttpToken(frame->request.method));
          CHECK(!mitto::IsForbiddenMethod(frame->request.method));
          CHECK_EQ(frame->request.url.size() + frame->request.method.size() +
                       frame->request.headers.size() +
                       frame->request.body.value_or("").size() + 31,
                   payload.size());
          break;
        case mitto::PageMessage::kCredit:
          CHECK_GT(frame->credit.bytes, 0u);
          CHECK_LE(frame->credit.bytes, mitto::kBodyWindow);
          break;
        case mitto::PageMessage::kGetCookies:
          CHECK_EQ(payload.size(), 16u);
          break;
        case mitto::PageMessage::kSetCookie:
          CHECK(base::IsStringUTF8AllowingNoncharacters(frame->cookie_line));
          break;
        case mitto::PageMessage::kLoaded:
          CHECK_EQ(payload.size(), 8u);
          break;
        case mitto::PageMessage::kCancel:
          CHECK_EQ(payload.size(), 8u);
          break;
        case mitto::PageMessage::kKnownGap:
          CHECK(base::IsStringUTF8AllowingNoncharacters(frame->known_gap));
          CHECK_EQ(frame->known_gap.size(), payload.size());
          break;
        case mitto::PageMessage::kCookieStoreGet: {
          const mitto::PageCookieStoreGet& get = frame->cookie_store_get;
          CHECK(base::IsStringUTF8AllowingNoncharacters(get.name));
          CHECK(!get.all || get.name.empty());
          CHECK_EQ(get.name.size() + 17, payload.size());
          break;
        }
        case mitto::PageMessage::kCookieStoreSet: {
          const mitto::PageCookieStoreSet& set = frame->cookie_store_set;
          CHECK(base::IsStringUTF8AllowingNoncharacters(set.name));
          CHECK(base::IsStringUTF8AllowingNoncharacters(set.value));
          CHECK(base::IsStringUTF8AllowingNoncharacters(set.domain));
          CHECK(base::IsStringUTF8AllowingNoncharacters(set.path));
          CHECK_NE(set.same_site, mitto::PageCookieSameSite::kNone);
          CHECK_LE(
              static_cast<uint8_t>(set.same_site),
              static_cast<uint8_t>(mitto::PageCookieSameSite::kNoRestriction));
          CHECK_EQ(set.name.size() + set.value.size() + set.domain.size() +
                       set.path.size() + 38,
                   payload.size());
          break;
        }
        case mitto::PageMessage::kCookieStoreListen:
          CHECK_EQ(payload.size(), 9u);
          break;
        case mitto::PageMessage::kBroadcast: {
          const mitto::PageBroadcast& broadcast = frame->broadcast;
          CHECK(base::IsStringUTF8AllowingNoncharacters(broadcast.name));
          CHECK(mitto::BroadcastFits(broadcast));
          CHECK_EQ(broadcast.name.size() + broadcast.data.size() + 13,
                   payload.size());
          break;
        }
        case mitto::PageMessage::kCacheStorage:
          // What the reader takes is exactly what the writer writes, after
          // the document's ID.
          CHECK_EQ(mitto::EncodeCacheStorageCall(frame->cache_storage_call),
                   std::string_view(payload).substr(8));
          break;
        case mitto::PageMessage::kDocumentCommitted: {
          const mitto::PageDocumentCommitted& committed = frame->committed;
          CHECK_NE(committed.document, mitto::kNoDocument);
          CHECK_EQ(
              payload.size(),
              committed.source == mitto::PageDocumentSource::kNavigationResponse
                  ? 17u
                  : 9u);
          break;
        }
        case mitto::PageMessage::kStart:
        case mitto::PageMessage::kEvaluate:
        case mitto::PageMessage::kResponseStarted:
        case mitto::PageMessage::kResponseBody:
        case mitto::PageMessage::kResponseEnded:
        case mitto::PageMessage::kCookies:
        case mitto::PageMessage::kCookieStoreCookies:
        case mitto::PageMessage::kCookieStoreStored:
        case mitto::PageMessage::kCookieStoreChange:
        case mitto::PageMessage::kBroadcastReceived:
        case mitto::PageMessage::kCacheStorageAnswer:
          // Only a Session sends these.
          NOTREACHED();
      }
    }
  }
  return 0;
}
