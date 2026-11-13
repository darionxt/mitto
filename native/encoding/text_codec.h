// A decoder of one of the Encoding Standard's encodings, as Blink's TextCodec
// is: it keeps what a stream leaves of a sequence from one call to the next,
// and the registry makes one for each encoding (text_encoding_registry.h).
// Only decoding is here, which TextDecoder needs. TextEncoder encodes UTF-8
// alone (text_codec_utf8.h).
//
// Upstream: third_party/blink/renderer/platform/wtf/text/text_codec.h
// Upstream: third_party/blink/renderer/platform/wtf/text/text_codec.cc

#ifndef MITTO_ENCODING_TEXT_CODEC_H_
#define MITTO_ENCODING_TEXT_CODEC_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <string_view>

#include "base/containers/span.h"
#include "base/functional/function_ref.h"

namespace mitto {

// Whether a call ends the stream, as Blink's FlushBehavior says.
enum class FlushBehavior {
  // More bytes may come.
  kDoNotFlush,
  // The end of a fetch's body, which the UTF-16 decoder does not take as the
  // end, as Blink's does not.
  kFetchEof,
  // The end of the data.
  kDataEof,
};

// U+FFFD, which a decoder gives for each error unless it stops at one.
inline constexpr char16_t kReplacementCharacter = 0xFFFD;

class TextCodec {
 public:
  TextCodec() = default;
  TextCodec(const TextCodec&) = delete;
  TextCodec& operator=(const TextCodec&) = delete;
  virtual ~TextCodec() = default;

  // Decodes `data`, after what earlier calls left of a sequence. With
  // `stop_on_error` it stops at the first error, and gives what it decoded
  // before it. Either way, an error sets `saw_error`.
  virtual std::u16string Decode(base::span<const uint8_t> data,
                                FlushBehavior flush,
                                bool stop_on_error,
                                bool& saw_error) = 0;
};

// What each codec gives the registry: the labels of its encodings, each with
// its encoding's canonical name, and a function that makes a decoder of an
// encoding from its canonical name.
using EncodingNameRegistrar =
    base::FunctionRef<void(std::string_view alias,
                           std::string_view canonical_name)>;
using NewTextCodecFunction =
    std::unique_ptr<TextCodec> (*)(std::string_view canonical_name);
using TextCodecRegistrar =
    base::FunctionRef<void(std::string_view canonical_name,
                           NewTextCodecFunction function)>;

// Appends a code point, as two code units if it is beyond the Basic
// Multilingual Plane, as WTF's StringBuilder::Append(UChar32) does.
void AppendCodePoint(std::u16string& result, int32_t code_point);

}  // namespace mitto

#endif  // MITTO_ENCODING_TEXT_CODEC_H_
