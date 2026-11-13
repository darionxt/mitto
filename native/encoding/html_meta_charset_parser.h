// The prescan of a document for a <meta> that names its encoding, as Blink's
// HTMLMetaCharsetParser runs it: the HTML tokenizer over the bytes read as
// windows-1252, the tokenizer's state switched for the tags that hold text, as
// the tree builder would, until a <meta> with a charset, or with
// http-equiv="Content-Type" and a content that names one, or a tag that the
// head cannot hold once 1,024 bytes have been read. Tags that look like tags
// inside <title>, <script> and <noscript> do not count.
//
// Upstream: third_party/blink/renderer/core/html/parser/html_meta_charset_parser.h
// Upstream: third_party/blink/renderer/core/html/parser/html_meta_charset_parser.cc
// Upstream: third_party/blink/renderer/core/html/parser/html_parser_idioms.cc

#ifndef MITTO_ENCODING_HTML_META_CHARSET_PARSER_H_
#define MITTO_ENCODING_HTML_META_CHARSET_PARSER_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"

namespace mitto {

class HtmlTokenizer;
class TextCodec;
struct HtmlAttribute;

class HtmlMetaCharsetParser {
 public:
  HtmlMetaCharsetParser();
  HtmlMetaCharsetParser(const HtmlMetaCharsetParser&) = delete;
  HtmlMetaCharsetParser& operator=(const HtmlMetaCharsetParser&) = delete;
  ~HtmlMetaCharsetParser();

  // Reads the next piece of the document's bytes. Returns true once it is done
  // checking, whether it found an encoding or not.
  bool CheckForMetaCharset(base::span<const uint8_t> data);
  // The document has ended.
  void Finish();

  // The canonical name of the encoding that a <meta> named, or empty.
  const std::string& encoding() const { return encoding_; }

 private:
  // Takes the tokens of what the tokenizer has. Before the first 1,023 bytes
  // have all been read, no token has been read past the first 1,024, so
  // `past_unconditional_bytes` is false until then. Returns whether it is
  // done.
  bool TakeTokens(bool past_unconditional_bytes);

  std::unique_ptr<HtmlTokenizer> tokenizer_;
  std::unique_ptr<TextCodec> assumed_codec_;
  // How many bytes the tokenizer has had.
  size_t bytes_read_ = 0;
  bool in_head_section_ = true;
  bool done_checking_ = false;
  std::string encoding_;
};

// Blink's EncodingFromMetaAttributes(): the canonical name of the encoding that
// a <meta> with `attributes` names, or empty.
std::string EncodingFromMetaAttributes(
    const std::vector<HtmlAttribute>& attributes);

// Blink's ExtractCharset(): the value of the first "charset=" in a <meta>'s
// content, or an empty string.
std::u16string_view ExtractCharset(std::u16string_view value);

}  // namespace mitto

#endif  // MITTO_ENCODING_HTML_META_CHARSET_PARSER_H_
