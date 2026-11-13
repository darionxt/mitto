// The encodings that ICU decodes and encodes for Blink, its TextCodecIcu: the
// Encoding Standard's single-byte encodings beyond windows-1252, such as
// ISO-8859-2, windows-1251 and KOI8-R, from the converters and the table of
// names that Chromium's ICU data holds, which the Page program holds too.
// Their names come from that table: each converter's name in the HTML standard
// of Chromium's data, or else its MIME name, with every alias of the converter
// but those that end in "-html". The encoder writes what UnencodableHandling
// says for a character that the converter lacks, and ICU's escape for an
// invalid one, such as "%UD800" for a lone surrogate.
//
// Blink keeps one closed converter for each thread to open the next quicker.
// The single-byte converters keep no state between calls, so a page sees no
// difference, and this opens one for each decoder.
//
// Upstream: third_party/blink/renderer/platform/wtf/text/text_codec_icu.h
// Upstream: third_party/blink/renderer/platform/wtf/text/text_codec_icu.cc

#ifndef MITTO_ENCODING_TEXT_CODEC_ICU_H_
#define MITTO_ENCODING_TEXT_CODEC_ICU_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <string_view>

#include "base/containers/span.h"
#include "mitto/encoding/text_codec.h"

// ICU's converter, which ucnv.h declares.
struct UConverter;

namespace mitto {

class TextCodecIcu final : public TextCodec {
 public:
  static std::unique_ptr<TextCodec> Create(std::string_view canonical_name);
  static void RegisterEncodingNames(EncodingNameRegistrar registrar);
  static void RegisterCodecs(TextCodecRegistrar registrar);

  explicit TextCodecIcu(std::string_view canonical_name);
  ~TextCodecIcu() override;

  std::u16string Decode(base::span<const uint8_t> data,
                        FlushBehavior flush,
                        bool stop_on_error,
                        bool& saw_error) override;
  std::string Encode(std::u16string_view text,
                     UnencodableHandling handling) override;

 private:
  struct ConverterCloser {
    void operator()(UConverter* converter) const;
  };

  // Opens the converter, which decoding and encoding share, as Blink's does.
  void CreateIcuConverter();
  std::string EncodeInternal(std::u16string_view text,
                             UnencodableHandling handling);

  const std::string encoding_;
  std::unique_ptr<UConverter, ConverterCloser> converter_;
};

}  // namespace mitto

#endif  // MITTO_ENCODING_TEXT_CODEC_ICU_H_
