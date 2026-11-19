#include "mitto/dom/text_decoder_stream.h"

#include <string_view>
#include <utility>

#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_text_decoder_stream.h"
#include "mitto/dom/text_decoder_transformer.h"
#include "mitto/dom/transform_stream.h"
#include "mitto/encoding/text_encoding_registry.h"

namespace mitto {

namespace {

// The Encoding Standard's ASCII whitespace, which a label may have around it.
constexpr char16_t kAsciiWhitespace[] = u"\t\n\f\r ";

}  // namespace

// static
TextDecoderStream* TextDecoderStream::Create(
    const std::u16string& label,
    const bindings::TextDecoderOptions& options,
    bindings::ExceptionState& exception_state) {
  const std::string encoding = CanonicalTextEncodingName(base::TrimString(
      std::u16string_view(label), kAsciiWhitespace, base::TRIM_ALL));
  // The Encoding API refuses the replacement encoding's labels too.
  if (encoding.empty() ||
      base::EqualsCaseInsensitiveASCII(encoding, "replacement")) {
    exception_state.ThrowRangeError(
        base::StrCat({"The encoding label provided ('",
                      base::UTF16ToUTF8(label), "') is invalid."}));
    return nullptr;
  }
  TransformStream* const transform = TransformStream::Create(
      exception_state.GetIsolate(),
      bindings::MakeGarbageCollected<TextDecoderTransformer>(
          encoding, options.fatal, options.ignore_bom),
      exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  return bindings::MakeGarbageCollected<TextDecoderStream>(
      transform, encoding, options.fatal, options.ignore_bom);
}

TextDecoderStream::TextDecoderStream(TransformStream* transform,
                                     std::string encoding,
                                     bool fatal,
                                     bool ignore_bom)
    : transform_(transform),
      encoding_(std::move(encoding)),
      fatal_(fatal),
      ignore_bom_(ignore_bom) {}

TextDecoderStream::~TextDecoderStream() = default;

const bindings::WrapperTypeInfo* TextDecoderStream::GetWrapperTypeInfo()
    const {
  return &bindings::V8TextDecoderStream::kWrapperTypeInfo;
}

void TextDecoderStream::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(transform_);
  bindings::ScriptWrappable::Trace(visitor);
}

std::u16string TextDecoderStream::Encoding() const {
  const std::string name = base::ToLowerASCII(encoding_);
  // As TextDecoder's encoding: ICU's names of Latin-1 and ASCII are
  // windows-1252 in Blink, which Chrome gives here too.
  if (name == "iso-8859-1" || name == "us-ascii") {
    return u"windows-1252";
  }
  return base::ASCIIToUTF16(name);
}

ReadableStream* TextDecoderStream::Readable() const {
  return transform_->Readable();
}

WritableStream* TextDecoderStream::Writable() const {
  return transform_->Writable();
}

}  // namespace mitto
