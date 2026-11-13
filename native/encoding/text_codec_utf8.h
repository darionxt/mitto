// UTF-8, as Blink's TextCodecUtf8 decodes and encodes it: each error gives one
// U+FFFD for the longest start of a sequence that could have been valid, as the
// Encoding Standard's UTF-8 decoder does, and a sequence that a call cuts off
// waits for the next.
//
// Blink decodes into 8-bit characters while it can, and into 16-bit ones from
// the first character beyond Latin-1. A page sees the same string either way,
// so this decodes into 16-bit characters only.
//
// Upstream: third_party/blink/renderer/platform/wtf/text/text_codec_utf8.h
// Upstream: third_party/blink/renderer/platform/wtf/text/text_codec_utf8.cc

#ifndef MITTO_ENCODING_TEXT_CODEC_UTF8_H_
#define MITTO_ENCODING_TEXT_CODEC_UTF8_H_

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <memory>
#include <string>
#include <string_view>

#include "base/containers/span.h"
#include "mitto/encoding/text_codec.h"

namespace mitto {

class TextCodecUtf8 final : public TextCodec {
 public:
  static std::unique_ptr<TextCodec> Create(std::string_view canonical_name);
  static bool IsSupported(std::string_view canonical_name);
  static void RegisterEncodingNames(EncodingNameRegistrar registrar);
  static void RegisterCodecs(TextCodecRegistrar registrar);

  TextCodecUtf8();
  ~TextCodecUtf8() override;

  std::u16string Decode(base::span<const uint8_t> data,
                        FlushBehavior flush,
                        bool stop_on_error,
                        bool& saw_error) override;
  std::string Encode(std::u16string_view text,
                     UnencodableHandling handling) override;

 private:
  void SavePartialSequenceBytes(base::span<const uint8_t>& source);
  void ConsumePartialSequenceBytes(size_t num_bytes);
  void FillPartialSequenceBytes(size_t sequence_length,
                                base::span<const uint8_t>& source);
  bool NeedMoreData(size_t sequence_length, int character, bool flush) const;
  void HandlePartialSequence(std::u16string& destination,
                             base::span<const uint8_t>& source,
                             bool flush,
                             bool stop_on_error,
                             bool& saw_error);

  // The bytes of a sequence that the last call cut off.
  size_t partial_sequence_size_ = 0;
  std::array<uint8_t, 4> partial_sequence_{};
};

// What EncodeUtf8Into() did.
struct Utf8EncodeIntoResult {
  // The code units of the source that it encoded.
  size_t code_units_read = 0;
  size_t bytes_written = 0;
};

// Encodes `text` as UTF-8, with U+FFFD for a lone surrogate.
std::string EncodeUtf8(std::u16string_view text);

// Encodes as much of `text` into `destination` as fits there, whole code points
// only, with U+FFFD for a lone surrogate.
Utf8EncodeIntoResult EncodeUtf8Into(std::u16string_view text,
                                    base::span<uint8_t> destination);

}  // namespace mitto

#endif  // MITTO_ENCODING_TEXT_CODEC_UTF8_H_
