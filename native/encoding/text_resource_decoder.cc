#include "mitto/encoding/text_resource_decoder.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/containers/span.h"
#include "base/strings/string_util.h"
#include "base/strings/string_view_util.h"
#include "mitto/encoding/html_meta_charset_parser.h"
#include "mitto/encoding/text_codec.h"
#include "mitto/encoding/text_encoding_detector.h"
#include "mitto/encoding/text_encoding_registry.h"

namespace mitto {

namespace {

using ContentType = TextResourceDecoderOptions::ContentType;
using EncodingDetection = TextResourceDecoderOptions::EncodingDetection;

// The shortest XML declaration that can name an encoding, as Blink waits for
// it: "<?xml" and three more bytes.
constexpr size_t kMinimumLengthOfXmlDeclaration = 8;

bool StartsWith(base::span<const uint8_t> bytes, std::string_view prefix) {
  return bytes.size() >= prefix.size() &&
         std::ranges::equal(bytes.first(prefix.size()),
                            base::as_byte_span(prefix));
}

// Blink's FindTextEncoding(): the encoding that the bytes of `name` label.
std::string FindTextEncoding(std::string_view name) {
  return CanonicalTextEncodingName(name);
}

// What decodes a resource that names no encoding: UTF-8 for XML and JSON,
// whatever the caller says, as Firefox does, and otherwise `default_encoding`,
// or windows-1252 if that is no encoding.
std::string DefaultEncoding(ContentType content_type,
                            std::string_view default_encoding) {
  if (content_type == ContentType::kXml || content_type == ContentType::kJson) {
    return std::string(kUtf8EncodingName);
  }
  if (default_encoding.empty()) {
    return std::string(kWindows1252EncodingName);
  }
  return std::string(default_encoding);
}

// The value of encoding="..." in an XML declaration, as Blink finds it, or an
// empty string.
std::string_view FindXmlEncoding(std::string_view str) {
  size_t pos = str.find("encoding");
  if (pos == std::string_view::npos) {
    return {};
  }
  pos += 8;
  // Spaces and stray control characters.
  while (pos < str.size() && static_cast<uint8_t>(str[pos]) <= ' ') {
    ++pos;
  }
  if (pos >= str.size() || str[pos] != '=') {
    return {};
  }
  ++pos;
  while (pos < str.size() && static_cast<uint8_t>(str[pos]) <= ' ') {
    ++pos;
  }
  if (pos >= str.size()) {
    return {};
  }
  const char quote_mark = str[pos];
  if (quote_mark != '"' && quote_mark != '\'') {
    return {};
  }
  ++pos;
  size_t end = pos;
  while (end < str.size() && str[end] != quote_mark) {
    ++end;
  }
  if (end >= str.size()) {
    return {};
  }
  return str.substr(pos, end - pos);
}

}  // namespace

TextResourceDecoderOptions::TextResourceDecoderOptions(
    ContentType content_type,
    std::string_view default_encoding)
    : content_type(content_type), default_encoding(default_encoding) {}

// static
TextResourceDecoderOptions TextResourceDecoderOptions::CreateWithAutoDetection(
    ContentType content_type,
    std::string_view default_encoding,
    std::string_view hint_encoding,
    const GURL& hint_url,
    std::string_view hint_language) {
  TextResourceDecoderOptions options(content_type, default_encoding);
  options.detection = EncodingDetection::kAllAutoDetection;
  options.hint_encoding = std::string(hint_encoding);
  options.hint_url = hint_url;
  options.hint_language = std::string(hint_language.substr(0, 2));
  return options;
}

// static
TextResourceDecoderOptions TextResourceDecoderOptions::CreateUtf8Decode() {
  TextResourceDecoderOptions options(ContentType::kPlainText,
                                     kUtf8EncodingName);
  options.detection = EncodingDetection::kAlwaysUtf8;
  return options;
}

TextResourceDecoderOptions::TextResourceDecoderOptions(
    const TextResourceDecoderOptions&) = default;
TextResourceDecoderOptions& TextResourceDecoderOptions::operator=(
    const TextResourceDecoderOptions&) = default;
TextResourceDecoderOptions::~TextResourceDecoderOptions() = default;

TextResourceDecoder::TextResourceDecoder(
    const TextResourceDecoderOptions& options)
    : options_(options),
      encoding_(
          DefaultEncoding(options.content_type, options.default_encoding)) {
  if (options_.detection == EncodingDetection::kAlwaysUtf8) {
    DCHECK(options_.content_type == ContentType::kPlainText);
    DCHECK_EQ(encoding_, kUtf8EncodingName);
  }
}

TextResourceDecoder::~TextResourceDecoder() = default;

void TextResourceDecoder::AddToBufferIfEmpty(base::span<const uint8_t> data) {
  if (buffer_.empty()) {
    buffer_.assign(data.begin(), data.end());
  }
}

void TextResourceDecoder::SetEncoding(std::string_view encoding,
                                      EncodingSource source) {
  // A page that names an encoding there is no such thing as keeps the one it
  // has.
  if (encoding.empty()) {
    return;
  }
  if (options_.detection == EncodingDetection::kAlwaysUtf8) {
    return;
  }
  // A <meta> cannot name x-user-defined, which is windows-1252 there, nor
  // an encoding that is not byte based, and neither can an XML declaration
  // or a style sheet's @charset.
  if (source == EncodingSource::kEncodingFromMetaTag &&
      encoding == "x-user-defined") {
    encoding_ = std::string(kWindows1252EncodingName);
  } else if (source == EncodingSource::kEncodingFromMetaTag ||
             source == EncodingSource::kEncodingFromXmlHeader ||
             source == EncodingSource::kEncodingFromCssCharset) {
    encoding_ = std::string(ClosestByteBasedEquivalent(encoding));
  } else {
    encoding_ = std::string(encoding);
  }
  codec_.reset();
  source_ = source;
}

size_t TextResourceDecoder::CheckForBom(base::span<const uint8_t> data) {
  // A byte order mark of UTF-8 or UTF-16 is a sure sign of its encoding, which
  // wins over any other, the user's choice too.
  DCHECK(!checked_for_bom_);
  if (data.size() < 2) {
    return 0;
  }
  const uint8_t c1 = data[0];
  const uint8_t c2 = data[1];
  const uint8_t c3 = data.size() >= 3 ? data[2] : 0;

  size_t length_of_bom = 0;
  if (c1 == 0xEF && c2 == 0xBB && c3 == 0xBF) {
    SetEncoding(kUtf8EncodingName, EncodingSource::kAutoDetectedEncoding);
    length_of_bom = 3;
  } else if (options_.detection != EncodingDetection::kAlwaysUtf8) {
    if (c1 == 0xFE && c2 == 0xFF) {
      SetEncoding(kUtf16BigEndianEncodingName,
                  EncodingSource::kAutoDetectedEncoding);
      length_of_bom = 2;
    } else if (c1 == 0xFF && c2 == 0xFE) {
      SetEncoding(kUtf16LittleEndianEncodingName,
                  EncodingSource::kAutoDetectedEncoding);
      length_of_bom = 2;
    }
  }

  constexpr size_t kMaxBomLength = 3;
  if (length_of_bom || data.size() >= kMaxBomLength) {
    checked_for_bom_ = true;
  }
  return length_of_bom;
}

bool TextResourceDecoder::CheckForCssCharset(base::span<const uint8_t> data) {
  if (source_ != EncodingSource::kDefaultEncoding &&
      source_ != EncodingSource::kEncodingFromParentFrame) {
    checked_for_css_charset_ = true;
    return true;
  }
  // The length of '@charset "x";'.
  if (data.size() <= 13) {
    return false;
  }
  if (StartsWith(data, "@charset \"")) {
    data = data.subspan(10u);
    const auto it = std::ranges::find(data, '"');
    if (it == data.end()) {
      return false;
    }
    const size_t encoding_name_length =
        static_cast<size_t>(std::distance(data.begin(), it));
    if (encoding_name_length + 1 >= data.size()) {
      return false;
    }
    if (data[encoding_name_length + 1] == ';') {
      SetEncoding(FindTextEncoding(
                      base::as_string_view(data.first(encoding_name_length))),
                  EncodingSource::kEncodingFromCssCharset);
    }
  }
  checked_for_css_charset_ = true;
  return true;
}

bool TextResourceDecoder::CheckForXmlCharset(base::span<const uint8_t> data) {
  if (source_ != EncodingSource::kDefaultEncoding &&
      source_ != EncodingSource::kEncodingFromParentFrame) {
    checked_for_xml_charset_ = true;
    return true;
  }
  if (data.size() < kMinimumLengthOfXmlDeclaration) {
    return false;
  }
  // An XML declaration names an encoding even for an HTML document, but only
  // at the very start.
  if (StartsWith(data, "<?xml")) {
    const auto it = std::ranges::find(data, '>');
    if (it == data.end()) {
      return false;
    }
    const size_t search_length =
        static_cast<size_t>(std::distance(data.begin(), it));
    const std::string_view encoding_name =
        FindXmlEncoding(base::as_string_view(data.first(search_length)));
    if (!encoding_name.empty()) {
      SetEncoding(FindTextEncoding(encoding_name),
                  EncodingSource::kEncodingFromXmlHeader);
    }
    // A <meta http-equiv> may still name one.
  } else if (StartsWith(data, std::string_view("<\0?\0x\0", 6))) {
    SetEncoding(kUtf16LittleEndianEncodingName,
                EncodingSource::kAutoDetectedEncoding);
  } else if (StartsWith(data, std::string_view("\0<\0?\0x", 6))) {
    SetEncoding(kUtf16BigEndianEncodingName,
                EncodingSource::kAutoDetectedEncoding);
  }
  checked_for_xml_charset_ = true;
  return true;
}

void TextResourceDecoder::CheckForMetaCharset(base::span<const uint8_t> data) {
  if (source_ == EncodingSource::kEncodingFromHttpHeader ||
      source_ == EncodingSource::kAutoDetectedEncoding) {
    checked_for_meta_charset_ = true;
    return;
  }
  if (!charset_parser_) {
    charset_parser_ = std::make_unique<HtmlMetaCharsetParser>();
  }
  if (!charset_parser_->CheckForMetaCharset(data)) {
    return;
  }
  FinalizeMetaCharsetCheck();
}

void TextResourceDecoder::FinalizeMetaCharsetCheck() {
  DCHECK(charset_parser_);
  SetEncoding(charset_parser_->encoding(),
              EncodingSource::kEncodingFromMetaTag);
  charset_parser_.reset();
  checked_for_meta_charset_ = true;
}

void TextResourceDecoder::AutoDetectEncodingIfAllowed(
    base::span<const uint8_t> data) {
  if (options_.detection != EncodingDetection::kAllAutoDetection ||
      detection_completed_) {
    return;
  }
  // The detector runs when nothing else has said, or when the encoding is a
  // parent frame's that its detector gave too, which is the hint then. A
  // frame takes its parent's encoding only if it can access the parent.
  if (!(source_ == EncodingSource::kDefaultEncoding ||
        (source_ == EncodingSource::kEncodingFromParentFrame &&
         !options_.hint_encoding.empty()))) {
    return;
  }
  std::string detected_encoding;
  if (DetectTextEncoding(data, options_.hint_encoding, options_.hint_url,
                         options_.hint_language, detected_encoding)) {
    // Chrome no longer takes ISO-2022-JP from the detector for a document,
    // with the runtime-enabled feature
    // RemoveCharsetAutoDetectionForISO2022JP, stable in the Engine Release.
    if ((options_.content_type == ContentType::kHtml ||
         options_.content_type == ContentType::kXml) &&
        detected_encoding == "ISO-2022-JP") {
      return;
    }
    SetEncoding(detected_encoding,
                EncodingSource::kEncodingFromContentSniffing);
  }
  // The detector runs again on the next piece until it names an encoding the
  // registry knows, even one it does not use, such as UTF-8.
  if (!detected_encoding.empty()) {
    detection_completed_ = true;
  }
}

std::u16string TextResourceDecoder::Decode(base::span<const uint8_t> data) {
  // What came before and waits goes first: a case that needs more bytes to
  // tell keeps the first piece, with AddToBufferIfEmpty().
  if (!buffer_.empty()) {
    buffer_.insert(buffer_.end(), data.begin(), data.end());
    data = base::span(buffer_);
  }

  size_t length_of_bom = 0;
  if (!checked_for_bom_) {
    length_of_bom = CheckForBom(data);
    if (!checked_for_bom_) {
      DCHECK_EQ(0u, length_of_bom);
      AddToBufferIfEmpty(data);
      return std::u16string();
    }
  }
  DCHECK_LE(length_of_bom, data.size());

  const ContentType content_type = options_.content_type;
  if (content_type == ContentType::kCss && !checked_for_css_charset_) {
    if (!CheckForCssCharset(data)) {
      AddToBufferIfEmpty(data);
      return std::u16string();
    }
  }
  if ((content_type == ContentType::kHtml ||
       content_type == ContentType::kXml) &&
      !checked_for_xml_charset_) {
    if (!CheckForXmlCharset(data)) {
      AddToBufferIfEmpty(data);
      return std::u16string();
    }
  }

  const base::span<const uint8_t> data_for_decode = data.subspan(length_of_bom);
  if (content_type == ContentType::kHtml && !checked_for_meta_charset_) {
    CheckForMetaCharset(data_for_decode);
  }
  AutoDetectEncodingIfAllowed(data);

  DCHECK(!encoding_.empty());
  if (!codec_) {
    codec_ = NewTextCodec(encoding_);
  }
  // An XML document stops at the first byte that its encoding cannot decode.
  std::u16string result =
      codec_->Decode(data_for_decode, FlushBehavior::kDoNotFlush,
                     content_type == ContentType::kXml, saw_error_);
  buffer_.clear();
  return result;
}

std::u16string TextResourceDecoder::Flush() {
  // A body that ended before the declarations at its start could be told
  // apart still gets the detector, if it may run.
  const ContentType content_type = options_.content_type;
  if (!buffer_.empty() &&
      ((!checked_for_xml_charset_ && (content_type == ContentType::kHtml ||
                                      content_type == ContentType::kXml)) ||
       (!checked_for_css_charset_ && content_type == ContentType::kCss))) {
    AutoDetectEncodingIfAllowed(buffer_);
  }
  if (content_type == ContentType::kHtml && !checked_for_meta_charset_ &&
      charset_parser_) {
    charset_parser_->Finish();
    FinalizeMetaCharsetCheck();
  }
  if (!codec_) {
    codec_ = NewTextCodec(encoding_);
  }
  std::u16string result =
      codec_->Decode(buffer_, FlushBehavior::kFetchEof,
                     content_type == ContentType::kXml, saw_error_);
  buffer_.clear();
  codec_.reset();
  // A byte order mark goes again when the body is decoded again.
  checked_for_bom_ = false;
  return result;
}

}  // namespace mitto
