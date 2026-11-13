#include "mitto/encoding/url_with_encoding.h"

#include <memory>
#include <utility>

#include "mitto/encoding/text_codec.h"
#include "mitto/encoding/text_encoding_registry.h"
#include "url/url_canon.h"
#include "url/url_canon_stdstring.h"
#include "url/url_util.h"

namespace mitto {

namespace {

// Blink's KURLCharsetConverter: the query in the document's encoding.
class EncodingCharsetConverter final : public url::CharsetConverter {
 public:
  explicit EncodingCharsetConverter(std::string_view encoding)
      : encoding_(encoding) {}

  void ConvertFromUtf16(std::u16string_view input,
                        url::CanonOutput* output) override {
    // TextEncoding::Encode() gives nothing for an empty string.
    if (input.empty()) {
      return;
    }
    output->Append(NewTextCodec(encoding_)->Encode(
        input, UnencodableHandling::kUrlEncodedCharRef));
  }

 private:
  const std::string encoding_;
};

}  // namespace

GURL ResolveUrlWithEncoding(const GURL& base,
                            std::u16string_view relative,
                            std::string_view encoding) {
  // UTF-8 needs no converter, and UTF-16 encodes as UTF-8, as Blink's
  // IsUnicodeEncoding() has it.
  std::unique_ptr<EncodingCharsetConverter> converter;
  if (!encoding.empty() &&
      ClosestByteBasedEquivalent(encoding) != kUtf8EncodingName) {
    converter = std::make_unique<EncodingCharsetConverter>(encoding);
  }
  // GURL::Resolve(), with the converter. An invalid base counts as none, as
  // Blink's null KURL does: only an absolute URL resolves, its query still in
  // the encoding.
  const std::string_view base_spec =
      base.is_valid() ? std::string_view(base.spec()) : std::string_view();
  const url::Parsed base_parsed =
      base.is_valid() ? base.parsed_for_possibly_invalid_spec() : url::Parsed();
  std::string spec;
  url::StdStringCanonOutput output(&spec);
  url::Parsed parsed;
  if (!url::ResolveRelative(base_spec, base_parsed, relative, converter.get(),
                            &output, &parsed)) {
    return GURL();
  }
  output.Complete();
  return GURL(std::move(spec), parsed, /*is_valid=*/true);
}

}  // namespace mitto
