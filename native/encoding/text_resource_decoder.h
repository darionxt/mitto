// Decodes the body of a response as it arrives, as Blink's TextResourceDecoder
// does for a document and a script: it works out the encoding as the bytes
// come, from a byte order mark, then for a document from an XML declaration at
// the start and a <meta> that the prescan finds (html_meta_charset_parser.h),
// then from Chrome's detector (text_encoding_detector.h), unless the response
// or the caller said which encoding it is. Each piece is decoded as soon as
// it comes, with what is known by then, as in Chrome: a <meta> found in a
// later piece changes the encoding of what follows it.
//
// The encoding is a canonical name of the registry
// (text_encoding_registry.h), and an empty one is no encoding, as Blink's
// invalid TextEncoding is.
//
// Upstream: third_party/blink/renderer/core/html/parser/text_resource_decoder.h
// Upstream: third_party/blink/renderer/core/html/parser/text_resource_decoder.cc
// Upstream: third_party/blink/renderer/platform/loader/fetch/text_resource_decoder_options.h
// Upstream: third_party/blink/renderer/platform/loader/fetch/text_resource_decoder_options.cc

#ifndef MITTO_ENCODING_TEXT_RESOURCE_DECODER_H_
#define MITTO_ENCODING_TEXT_RESOURCE_DECODER_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "url/gurl.h"

namespace mitto {

class HtmlMetaCharsetParser;
class TextCodec;

struct TextResourceDecoderOptions {
  // What the resource is, which decides where its encoding may be declared.
  enum class ContentType {
    // A script, or a document of a type that none of the others is.
    kPlainText,
    kHtml,
    kXml,
    kCss,
    kJson,
  };

  enum class EncodingDetection {
    // The byte order mark, and what the content declares.
    kContentAndBom,
    // Those, and Chrome's detector when nothing else says.
    kAllAutoDetection,
    // UTF-8 whatever the response says, but for a byte order mark of UTF-8,
    // as the Encoding Standard's UTF-8 decode: module scripts and workers.
    kAlwaysUtf8,
  };

  // Blink's TextResourceDecoderOptions(content_type, default_encoding).
  TextResourceDecoderOptions(ContentType content_type,
                             std::string_view default_encoding);
  // Blink's CreateWithAutoDetection(): the detector may run, with the encoding
  // of a parent frame as its hint, and `hint_url`'s top-level domain, which
  // decides with `hint_language` for a local file.
  static TextResourceDecoderOptions CreateWithAutoDetection(
      ContentType content_type,
      std::string_view default_encoding,
      std::string_view hint_encoding,
      const GURL& hint_url,
      std::string_view hint_language);
  // Blink's CreateUTF8Decode().
  static TextResourceDecoderOptions CreateUtf8Decode();

  TextResourceDecoderOptions(const TextResourceDecoderOptions&);
  TextResourceDecoderOptions& operator=(const TextResourceDecoderOptions&);
  ~TextResourceDecoderOptions();

  EncodingDetection detection = EncodingDetection::kContentAndBom;
  ContentType content_type = ContentType::kPlainText;
  // What decodes when nothing else says, which may be no encoding.
  std::string default_encoding;
  std::string hint_encoding;
  GURL hint_url;
  // The first two letters of the browser's language, for the detector.
  std::string hint_language;
};

class TextResourceDecoder {
 public:
  // Where the encoding came from, in Blink's order.
  enum class EncodingSource {
    kDefaultEncoding,
    kAutoDetectedEncoding,
    kEncodingFromContentSniffing,
    kEncodingFromXmlHeader,
    kEncodingFromMetaTag,
    kEncodingFromCssCharset,
    kEncodingFromHttpHeader,
    kEncodingFromParentFrame,
  };

  explicit TextResourceDecoder(const TextResourceDecoderOptions& options);
  TextResourceDecoder(const TextResourceDecoder&) = delete;
  TextResourceDecoder& operator=(const TextResourceDecoder&) = delete;
  ~TextResourceDecoder();

  // Decodes the rest as `encoding`, unless it is no encoding, which keeps the
  // one there is, as Blink keeps it for a page that names one it lacks.
  void SetEncoding(std::string_view encoding, EncodingSource source);
  const std::string& encoding() const { return encoding_; }
  bool EncodingWasDetectedHeuristically() const {
    return source_ == EncodingSource::kAutoDetectedEncoding ||
           source_ == EncodingSource::kEncodingFromContentSniffing;
  }
  bool saw_error() const { return saw_error_; }

  // Decodes the next piece of the body. It may keep some of it, and give an
  // empty string, until it has enough to see a byte order mark or a
  // declaration at the start.
  std::u16string Decode(base::span<const uint8_t> data);
  // What is left at the end of the body. A later Decode() starts again, as for
  // a new body.
  std::u16string Flush();

 private:
  // Returns how long the byte order mark is, after it has set the encoding it
  // names, or 0. It keeps checking until three bytes have come.
  size_t CheckForBom(base::span<const uint8_t> data);
  // Return false if more bytes are needed to tell.
  bool CheckForCssCharset(base::span<const uint8_t> data);
  bool CheckForXmlCharset(base::span<const uint8_t> data);
  void CheckForMetaCharset(base::span<const uint8_t> data);
  void FinalizeMetaCharsetCheck();
  void AutoDetectEncodingIfAllowed(base::span<const uint8_t> data);

  void AddToBufferIfEmpty(base::span<const uint8_t> data);

  const TextResourceDecoderOptions options_;

  std::string encoding_;
  std::unique_ptr<TextCodec> codec_;
  EncodingSource source_ = EncodingSource::kDefaultEncoding;
  // What came and waits until there is enough to tell.
  std::vector<uint8_t> buffer_;
  bool checked_for_bom_ = false;
  bool checked_for_css_charset_ = false;
  bool checked_for_xml_charset_ = false;
  bool checked_for_meta_charset_ = false;
  bool saw_error_ = false;
  bool detection_completed_ = false;
  std::unique_ptr<HtmlMetaCharsetParser> charset_parser_;
};

}  // namespace mitto

#endif  // MITTO_ENCODING_TEXT_RESOURCE_DECODER_H_
