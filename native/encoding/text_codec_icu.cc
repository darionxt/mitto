#include "mitto/encoding/text_codec_icu.h"

#include <array>

#include "base/check.h"
#include "base/containers/span.h"
#include "base/memory/raw_ptr.h"
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

std::u16string TextCodecIcu::Decode(base::span<const uint8_t> data,
                                    FlushBehavior flush,
                                    bool stop_on_error,
                                    bool& saw_error) {
  if (!converter_) {
    UErrorCode error = U_ZERO_ERROR;
    converter_.reset(ucnv_open(encoding_.c_str(), &error));
    if (!converter_) {
      return std::u16string();
    }
    ucnv_setFallback(converter_.get(), true);
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

}  // namespace mitto
