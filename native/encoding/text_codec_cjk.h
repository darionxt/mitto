// The Encoding Standard's CJK encodings, EUC-JP, ISO-2022-JP, Shift_JIS,
// EUC-KR, GBK, gb18030 and Big5, as Blink's TextCodecCjk decodes and encodes
// them: the Encoding Standard's decoders and encoders, over indexes built from
// ICU (encoding_tables.h).
//
// Upstream: third_party/blink/renderer/platform/wtf/text/text_codec_cjk.h
// Upstream: third_party/blink/renderer/platform/wtf/text/text_codec_cjk.cc

#ifndef MITTO_ENCODING_TEXT_CODEC_CJK_H_
#define MITTO_ENCODING_TEXT_CODEC_CJK_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <string_view>

#include "base/containers/span.h"
#include "mitto/encoding/text_codec.h"

namespace mitto {

class TextCodecCjk final : public TextCodec {
 public:
  class Decoder;
  enum class Encoding : uint8_t {
    kEucJp,
    kIso2022Jp,
    kShiftJis,
    kEucKr,
    kGbk,
    kGb18030,
    kBig5,
  };

  static std::unique_ptr<TextCodec> Create(std::string_view canonical_name);
  static bool IsSupported(std::string_view canonical_name);
  static void RegisterEncodingNames(EncodingNameRegistrar registrar);
  static void RegisterCodecs(TextCodecRegistrar registrar);

  explicit TextCodecCjk(Encoding encoding);
  ~TextCodecCjk() override;

  std::u16string Decode(base::span<const uint8_t> data,
                        FlushBehavior flush,
                        bool stop_on_error,
                        bool& saw_error) override;
  std::string Encode(std::u16string_view text,
                     UnencodableHandling handling) override;

 private:
  const Encoding encoding_;
  std::unique_ptr<Decoder> decoder_;
};

}  // namespace mitto

#endif  // MITTO_ENCODING_TEXT_CODEC_CJK_H_
