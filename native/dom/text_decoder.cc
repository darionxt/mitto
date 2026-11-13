#include "mitto/dom/text_decoder.h"

#include <utility>

#include "base/check.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_text_decoder.h"
#include "mitto/encoding/text_encoding_registry.h"

namespace mitto {

namespace {

// The Encoding Standard's ASCII whitespace, which a label may have around it.
constexpr char16_t kAsciiWhitespace[] = u"\t\n\f\r ";

// WTF's kStringMaxUCharLength: the longest string of 16-bit characters whose
// allocation, after a 12-byte header, fits in partition_alloc's largest, 2 GiB
// less a super page of 2 MiB.
constexpr size_t kStringMaxUCharLength =
    ((size_t{1} << 31) - (size_t{1} << 21) - 12) / sizeof(char16_t);

}  // namespace

// static
TextDecoder* TextDecoder::Create(const std::u16string& label,
                                 const bindings::TextDecoderOptions& options,
                                 bindings::ExceptionState& exception_state) {
  const std::string encoding = CanonicalTextEncodingName(base::TrimString(
      std::u16string_view(label), kAsciiWhitespace, base::TRIM_ALL));
  // The Encoding API refuses the replacement encoding's labels too, which name
  // no valid encoding for it.
  if (encoding.empty() ||
      base::EqualsCaseInsensitiveASCII(encoding, "replacement")) {
    exception_state.ThrowRangeError(
        base::StrCat({"The encoding label provided ('",
                      base::UTF16ToUTF8(label), "') is invalid."}));
    return nullptr;
  }
  return bindings::MakeGarbageCollected<TextDecoder>(encoding, options.fatal,
                                                     options.ignore_bom);
}

TextDecoder::TextDecoder(std::string encoding, bool fatal, bool ignore_bom)
    : encoding_(std::move(encoding)), fatal_(fatal), ignore_bom_(ignore_bom) {}

TextDecoder::~TextDecoder() = default;

const bindings::WrapperTypeInfo* TextDecoder::GetWrapperTypeInfo() const {
  return &bindings::V8TextDecoder::kWrapperTypeInfo;
}

std::u16string TextDecoder::Encoding() const {
  const std::string name = base::ToLowerASCII(encoding_);
  // ICU's names of Latin-1 and ASCII decode as windows-1252, and keep their own
  // names in Blink, which gives windows-1252 here.
  if (name == "iso-8859-1" || name == "us-ascii") {
    return u"windows-1252";
  }
  return base::ASCIIToUTF16(name);
}

std::u16string TextDecoder::Decode(
    std::optional<base::span<const uint8_t>> input,
    const bindings::TextDecodeOptions& options,
    bindings::ExceptionState& exception_state) {
  const base::span<const uint8_t> bytes =
      input.value_or(base::span<const uint8_t>());
  // Room for the three bytes of a sequence that UTF-8 may keep from before.
  if (bytes.size() > kStringMaxUCharLength - 3) {
    exception_state.ThrowRangeError(
        "Buffer size exceeds maximum heap object size.");
    return std::u16string();
  }

  if (!do_not_flush_) {
    // A new stream: the decoder of the last full one is reused, or made.
    if (!codec_) {
      codec_ = NewTextCodec(encoding_);
    }
    bom_seen_ = false;
  }
  do_not_flush_ = options.stream;
  const FlushBehavior flush =
      do_not_flush_ ? FlushBehavior::kDoNotFlush : FlushBehavior::kDataEof;

  bool saw_error = false;
  std::u16string text = codec_->Decode(bytes, flush, fatal_, saw_error);

  if (fatal_ && saw_error) {
    if (!do_not_flush_) {
      // A stream that ended keeps no error for the next.
      codec_.reset();
    }
    exception_state.ThrowTypeError("The encoded data was not valid.");
    return std::u16string();
  }

  if (!ignore_bom_ && !bom_seen_ && !text.empty()) {
    bom_seen_ = true;
    if (text[0] == 0xFEFF && (encoding_ == "UTF-8" || encoding_ == "UTF-16LE" ||
                              encoding_ == "UTF-16BE")) {
      text.erase(0, 1);
    }
  }
  return text;
}

}  // namespace mitto
