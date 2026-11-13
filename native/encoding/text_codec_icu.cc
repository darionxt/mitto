#include "mitto/encoding/text_codec_icu.h"

#include <array>

#include "base/check.h"
#include "base/containers/span.h"
#include "base/memory/raw_ptr.h"
#include "base/notreached.h"
#include "base/strings/utf_string_conversions.h"
#include "base/types/to_address.h"
#include "mitto/encoding/text_codec_cjk.h"
#include "mitto/encoding/text_codec_utf16.h"
#include "mitto/encoding/text_codec_utf8.h"
#include "third_party/icu/source/common/unicode/ucnv.h"
#include "third_party/icu/source/common/unicode/ucnv_cb.h"

namespace mitto {

namespace {

constexpr size_t kConversionBufferSize = 16384;

// Chromium's ICU has aliases that end in "-html" for the labels of the
// Encoding Standard, which a page must not see.
bool IncludeAlias(std::string_view alias) {
  return !alias.ends_with("-html");
}

// A converter's name in the HTML standard of Chromium's ICU data, or else its
// MIME name, or null for one that has neither.
const char* GetStandardName(const char* name) {
  UErrorCode error = U_ZERO_ERROR;
  const char* standard_name = ucnv_getStandardName(name, "HTML", &error);
  if (U_FAILURE(error) || !standard_name) {
    error = U_ZERO_ERROR;
    standard_name = ucnv_getStandardName(name, "MIME", &error);
    if (U_FAILURE(error) || !standard_name) {
      return nullptr;
    }
  }
  return standard_name;
}

// Whether another codec decodes the encoding.
bool ShouldSkipEncoding(std::string_view canonical_name) {
  return TextCodecCjk::IsSupported(canonical_name) ||
         TextCodecUtf16::IsSupported(canonical_name) ||
         TextCodecUtf8::IsSupported(canonical_name);
}

// Stops the converter at an invalid sequence while it lives, if `stop` says
// so, where it would give U+FFFD.
class ErrorCallbackSetter final {
 public:
  ErrorCallbackSetter(UConverter* converter, bool stop)
      : converter_(converter), stop_(stop) {
    if (stop_) {
      UErrorCode error = U_ZERO_ERROR;
      const void* saved_context = nullptr;
      ucnv_setToUCallBack(converter_, UCNV_TO_U_CALLBACK_STOP, nullptr,
                          &saved_action_, &saved_context, &error);
      saved_context_ = saved_context;
      DCHECK_EQ(error, U_ZERO_ERROR);
    }
  }
  ErrorCallbackSetter(const ErrorCallbackSetter&) = delete;
  ErrorCallbackSetter& operator=(const ErrorCallbackSetter&) = delete;
  ~ErrorCallbackSetter() {
    if (stop_) {
      UErrorCode error = U_ZERO_ERROR;
      const void* old_context;
      UConverterToUCallback old_action;
      ucnv_setToUCallBack(converter_, saved_action_, saved_context_.get(),
                          &old_action, &old_context, &error);
      DCHECK_EQ(error, U_ZERO_ERROR);
    }
  }

 private:
  const raw_ptr<UConverter> converter_;
  const bool stop_;
  raw_ptr<const void> saved_context_ = nullptr;
  UConverterToUCallback saved_action_ = nullptr;
};

// Decodes what of `source` fits into `target`, and takes it from `source`.
// Returns how many code units it wrote.
size_t DecodeToBuffer(UConverter* converter,
                      base::span<char16_t> target,
                      base::span<const uint8_t>& source,
                      bool flush,
                      UErrorCode& error) {
  const char* source_pointer = reinterpret_cast<const char*>(source.data());
  const char* source_end =
      reinterpret_cast<const char*>(base::to_address(source.end()));
  char16_t* target_pointer = target.data();
  error = U_ZERO_ERROR;
  ucnv_toUnicode(converter, &target_pointer, base::to_address(target.end()),
                 &source_pointer, source_end, nullptr, flush, &error);
  source = source.subspan(static_cast<size_t>(
      source_pointer - reinterpret_cast<const char*>(source.data())));
  return static_cast<size_t>(target_pointer - target.data());
}

// Writes what `handling` gives for a code point that the converter lacks.
// Otherwise, as for an invalid code point such as a lone surrogate, it does
// what ICU's escape callback does, which writes "%UD800" for U+D800.
void FormatEscapedEntityCallback(const void* context,
                                 UConverterFromUnicodeArgs* from_u_args,
                                 const UChar* code_units,
                                 int32_t length,
                                 UChar32 code_point,
                                 UConverterCallbackReason reason,
                                 UErrorCode* error,
                                 UnencodableHandling handling) {
  if (reason == UCNV_UNASSIGNED) {
    *error = U_ZERO_ERROR;
    const std::u16string entity = base::ASCIIToUTF16(
        TextCodec::GetUnencodableReplacement(code_point, handling));
    const char16_t* source = entity.data();
    ucnv_cbFromUWriteUChars(from_u_args, &source,
                            base::to_address(entity.end()), 0, error);
  } else {
    UCNV_FROM_U_CALLBACK_ESCAPE(context, from_u_args, code_units, length,
                                code_point, reason, error);
  }
}

void NumericEntityCallback(const void* context,
                           UConverterFromUnicodeArgs* from_u_args,
                           const UChar* code_units,
                           int32_t length,
                           UChar32 code_point,
                           UConverterCallbackReason reason,
                           UErrorCode* error) {
  FormatEscapedEntityCallback(context, from_u_args, code_units, length,
                              code_point, reason, error,
                              UnencodableHandling::kXmlCharRef);
}

void CssEscapedEntityCallback(const void* context,
                              UConverterFromUnicodeArgs* from_u_args,
                              const UChar* code_units,
                              int32_t length,
                              UChar32 code_point,
                              UConverterCallbackReason reason,
                              UErrorCode* error) {
  FormatEscapedEntityCallback(context, from_u_args, code_units, length,
                              code_point, reason, error,
                              UnencodableHandling::kCssEscape);
}

void UrlEscapedEntityCallback(const void* context,
                              UConverterFromUnicodeArgs* from_u_args,
                              const UChar* code_units,
                              int32_t length,
                              UChar32 code_point,
                              UConverterCallbackReason reason,
                              UErrorCode* error) {
  FormatEscapedEntityCallback(context, from_u_args, code_units, length,
                              code_point, reason, error,
                              UnencodableHandling::kUrlEncodedCharRef);
}

void NotReachedEntityCallback(const void* context,
                              UConverterFromUnicodeArgs* from_u_args,
                              const UChar* code_units,
                              int32_t length,
                              UChar32 code_point,
                              UConverterCallbackReason reason,
                              UErrorCode* error) {
  NOTREACHED();
}

}  // namespace

void TextCodecIcu::ConverterCloser::operator()(UConverter* converter) const {
  ucnv_close(converter);
}

// static
std::unique_ptr<TextCodec> TextCodecIcu::Create(
    std::string_view canonical_name) {
  return std::make_unique<TextCodecIcu>(canonical_name);
}

// static
void TextCodecIcu::RegisterEncodingNames(EncodingNameRegistrar registrar) {
  // Hebrew in logical order has a name of its own, which ICU takes as a
  // synonym of ISO-8859-8, so that the two stay apart.
  registrar("ISO-8859-8-I", "ISO-8859-8-I");

  const int32_t num_encodings = ucnv_countAvailable();
  for (int32_t i = 0; i < num_encodings; ++i) {
    const char* name = ucnv_getAvailableName(i);
    const char* standard_name = GetStandardName(name);
    if (!standard_name || ShouldSkipEncoding(standard_name)) {
      continue;
    }
    registrar(standard_name, standard_name);
    UErrorCode error = U_ZERO_ERROR;
    const uint16_t num_aliases = ucnv_countAliases(name, &error);
    if (U_FAILURE(error)) {
      continue;
    }
    for (uint16_t j = 0; j < num_aliases; ++j) {
      error = U_ZERO_ERROR;
      const char* alias = ucnv_getAlias(name, j, &error);
      if (U_SUCCESS(error) && IncludeAlias(alias)) {
        registrar(alias, standard_name);
      }
    }
  }

  // ICU's table cannot have both ISO-8859-8-I and ISO-8859-8.
  registrar("csISO88598I", "ISO-8859-8-I");
  registrar("logical", "ISO-8859-8-I");
}

// static
void TextCodecIcu::RegisterCodecs(TextCodecRegistrar registrar) {
  registrar("ISO-8859-8-I", &Create);
  const int32_t num_encodings = ucnv_countAvailable();
  for (int32_t i = 0; i < num_encodings; ++i) {
    const char* standard_name = GetStandardName(ucnv_getAvailableName(i));
    if (!standard_name || ShouldSkipEncoding(standard_name)) {
      continue;
    }
    registrar(standard_name, &Create);
  }
}

TextCodecIcu::TextCodecIcu(std::string_view canonical_name)
    : encoding_(canonical_name) {}

TextCodecIcu::~TextCodecIcu() = default;

void TextCodecIcu::CreateIcuConverter() {
  DCHECK(!converter_);
  UErrorCode error = U_ZERO_ERROR;
  converter_.reset(ucnv_open(encoding_.c_str(), &error));
  if (converter_) {
    ucnv_setFallback(converter_.get(), true);
  }
}

std::u16string TextCodecIcu::Decode(base::span<const uint8_t> data,
                                    FlushBehavior flush,
                                    bool stop_on_error,
                                    bool& saw_error) {
  if (!converter_) {
    CreateIcuConverter();
    if (!converter_) {
      return std::u16string();
    }
  }

  const ErrorCallbackSetter callback_setter(converter_.get(), stop_on_error);
  std::u16string result;
  std::array<char16_t, kConversionBufferSize> buffer;
  base::span<const uint8_t> source = data;
  UErrorCode error = U_ZERO_ERROR;
  do {
    const size_t decoded =
        DecodeToBuffer(converter_.get(), buffer, source,
                       flush != FlushBehavior::kDoNotFlush, error);
    const base::span<const char16_t> decoded_span =
        base::span(buffer).first(decoded);
    result.append(decoded_span.begin(), decoded_span.end());
  } while (error == U_BUFFER_OVERFLOW_ERROR);

  if (U_FAILURE(error)) {
    // Flushes the converter, so that the error does not reach the next call.
    do {
      DecodeToBuffer(converter_.get(), buffer, source, /*flush=*/true, error);
    } while (!source.empty());
    saw_error = true;
  }
  return result;
}

std::string TextCodecIcu::EncodeInternal(std::u16string_view text,
                                         UnencodableHandling handling) {
  UErrorCode error = U_ZERO_ERROR;
  switch (handling) {
    case UnencodableHandling::kXmlCharRef:
      ucnv_setFromUCallBack(converter_.get(), NumericEntityCallback, nullptr,
                            nullptr, nullptr, &error);
      break;
    case UnencodableHandling::kUrlEncodedCharRef:
      ucnv_setFromUCallBack(converter_.get(), UrlEscapedEntityCallback, nullptr,
                            nullptr, nullptr, &error);
      break;
    case UnencodableHandling::kCssEscape:
      ucnv_setFromUCallBack(converter_.get(), CssEscapedEntityCallback, nullptr,
                            nullptr, nullptr, &error);
      break;
    case UnencodableHandling::kNone:
      // Only UTF-8 and UTF-16 take kNone, and ICU encodes neither here.
      DCHECK(TextCodecUtf16::IsSupported(encoding_) ||
             TextCodecUtf8::IsSupported(encoding_));
      ucnv_setFromUCallBack(converter_.get(), NotReachedEntityCallback, nullptr,
                            nullptr, nullptr, &error);
      break;
  }
  DCHECK(U_SUCCESS(error));
  if (U_FAILURE(error)) {
    return std::string();
  }

  const base::span<const char16_t> input(text);
  const char16_t* source = input.data();
  std::string result;
  std::array<char, kConversionBufferSize> buffer;
  do {
    char* target = buffer.data();
    error = U_ZERO_ERROR;
    ucnv_fromUnicode(converter_.get(), &target, base::to_address(buffer.end()),
                     &source, base::to_address(input.end()), nullptr, true,
                     &error);
    const base::span<const char> encoded =
        base::span(buffer).first(static_cast<size_t>(target - buffer.data()));
    result.append(encoded.begin(), encoded.end());
  } while (error == U_BUFFER_OVERFLOW_ERROR);
  return result;
}

std::string TextCodecIcu::Encode(std::u16string_view text,
                                 UnencodableHandling handling) {
  if (text.empty()) {
    return std::string();
  }
  if (!converter_) {
    CreateIcuConverter();
  }
  if (!converter_) {
    return std::string();
  }
  return EncodeInternal(text, handling);
}

}  // namespace mitto
