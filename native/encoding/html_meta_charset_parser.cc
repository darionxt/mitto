#include "mitto/encoding/html_meta_charset_parser.h"

#include <algorithm>

#include "base/check.h"
#include "base/strings/string_util.h"
#include "mitto/encoding/text_codec.h"
#include "mitto/encoding/text_encoding_registry.h"
#include "mitto/html/html_token.h"
#include "mitto/html/html_tokenizer.h"

namespace mitto {

namespace {

// That many bytes are read for a <meta> even after the head has ended, since
// many pages declare their encoding after a tag that the head cannot hold.
constexpr size_t kBytesToCheckUnconditionally = 1024;

constexpr std::u16string_view kCharsetString = u"charset";

// The tags of the head and those that hold text which looks like tags, after
// which the prescan goes on: rather than at </head>, it stops at the first
// tag the head cannot hold, which matches other browsers more closely.
bool KeepsHeadSection(std::u16string_view tag, bool end) {
  for (const std::u16string_view allowed :
       {u"script", u"noscript", u"style", u"link", u"meta", u"object", u"title",
        u"base"}) {
    if (tag == allowed) {
      return true;
    }
  }
  return !end && (tag == u"html" || tag == u"head");
}

// Blink's HTMLTokenizer::UpdateStateFor(), with no scripting, as the prescan's
// tokenizer has it: the state the tree builder would switch to after the start
// tag `tag`, which reads <noscript> as markup.
void UpdateStateFor(HtmlTokenizer& tokenizer, std::u16string_view tag) {
  if (tag == u"textarea" || tag == u"title") {
    tokenizer.SetState(HtmlTokenizer::State::kRcdata);
  } else if (tag == u"plaintext") {
    tokenizer.SetState(HtmlTokenizer::State::kPlaintext);
  } else if (tag == u"script") {
    tokenizer.SetState(HtmlTokenizer::State::kScriptData);
  } else if (tag == u"style" || tag == u"iframe" || tag == u"xmp" ||
             tag == u"noembed" || tag == u"noframes") {
    tokenizer.SetState(HtmlTokenizer::State::kRawtext);
  }
}

bool IsHtmlSpace(char16_t c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

// Where `needle`, in lower case, is in `haystack` at `from` or after, whatever
// the ASCII case there, or npos.
size_t FindIgnoringAsciiCase(std::u16string_view haystack,
                             std::u16string_view needle,
                             size_t from) {
  if (needle.size() > haystack.size()) {
    return std::u16string_view::npos;
  }
  for (size_t i = from; i + needle.size() <= haystack.size(); ++i) {
    if (base::EqualsCaseInsensitiveASCII(haystack.substr(i, needle.size()),
                                         needle)) {
      return i;
    }
  }
  return std::u16string_view::npos;
}

}  // namespace

HtmlMetaCharsetParser::HtmlMetaCharsetParser()
    : tokenizer_(std::make_unique<HtmlTokenizer>()),
      assumed_codec_(NewTextCodec(kWindows1252EncodingName)) {}

HtmlMetaCharsetParser::~HtmlMetaCharsetParser() = default;

bool HtmlMetaCharsetParser::CheckForMetaCharset(
    base::span<const uint8_t> data) {
  if (done_checking_) {
    return true;
  }
  DCHECK(encoding_.empty());

  // windows-1252 gives one code unit for each byte, so a count of code units
  // is one of bytes, which Blink counts as characters the tokenizer consumed.
  bool saw_error = false;
  const std::u16string text = assumed_codec_->Decode(
      data, FlushBehavior::kDoNotFlush, /*stop_on_error=*/false, saw_error);
  std::u16string_view rest = text;

  // Blink stops at the first token after a tag that the head cannot hold once
  // the tokenizer has consumed 1,024 characters, and a token that ends there
  // counts. This tokenizer does not say how far it has consumed, so it gets
  // the first 1,023 bytes on their own: none of their tokens can stop the
  // prescan, and every token after them has consumed 1,024 or more.
  if (bytes_read_ < kBytesToCheckUnconditionally - 1) {
    const size_t first =
        std::min(rest.size(), kBytesToCheckUnconditionally - 1 - bytes_read_);
    tokenizer_->Append(rest.substr(0, first));
    bytes_read_ += first;
    rest.remove_prefix(first);
    if (TakeTokens(/*past_unconditional_bytes=*/false)) {
      return true;
    }
  }
  if (rest.empty()) {
    return false;
  }
  tokenizer_->Append(rest);
  bytes_read_ += rest.size();
  return TakeTokens(/*past_unconditional_bytes=*/true);
}

bool HtmlMetaCharsetParser::TakeTokens(bool past_unconditional_bytes) {
  HtmlToken token;
  while (tokenizer_->NextToken(token)) {
    const bool end = token.type == HtmlToken::Type::kEndTag;
    if (end || token.type == HtmlToken::Type::kStartTag) {
      if (!end) {
        UpdateStateFor(*tokenizer_, token.name);
        if (token.name == u"meta") {
          encoding_ = EncodingFromMetaAttributes(token.attributes);
          if (!encoding_.empty()) {
            done_checking_ = true;
            return true;
          }
        }
      }
      if (!KeepsHeadSection(token.name, end)) {
        in_head_section_ = false;
      }
    }
    if (!in_head_section_ && past_unconditional_bytes) {
      done_checking_ = true;
      return true;
    }
  }
  return false;
}

void HtmlMetaCharsetParser::Finish() {
  done_checking_ = true;
}

std::string EncodingFromMetaAttributes(
    const std::vector<HtmlAttribute>& attributes) {
  // The tokenizer keeps the first of two attributes with one name, where
  // Blink's prescan sees both, and the last of two charsets wins there.
  bool got_pragma = false;
  bool has_charset = false;
  enum class Mode { kNone, kCharset, kPragma } mode = Mode::kNone;
  std::u16string_view charset;
  for (const HtmlAttribute& attribute : attributes) {
    if (attribute.name == u"http-equiv") {
      if (base::EqualsCaseInsensitiveASCII(attribute.value, u"content-type")) {
        got_pragma = true;
      }
    } else if (attribute.name == u"charset") {
      has_charset = true;
      charset = attribute.value;
      mode = Mode::kCharset;
    } else if (!has_charset && attribute.name == u"content") {
      charset = ExtractCharset(attribute.value);
      if (!charset.empty()) {
        mode = Mode::kPragma;
      }
    }
  }
  if (mode == Mode::kCharset || (mode == Mode::kPragma && got_pragma)) {
    const auto first = std::ranges::find_if_not(charset, IsHtmlSpace);
    const auto last =
        std::ranges::find_if_not(charset.rbegin(), charset.rend(), IsHtmlSpace);
    if (first == charset.end()) {
      return std::string();
    }
    return CanonicalTextEncodingName(std::u16string_view(first, last.base()));
  }
  return std::string();
}

std::u16string_view ExtractCharset(std::u16string_view value) {
  const size_t length = value.size();
  // Past the end reads as U+0000, as WTF::String's operator[] does.
  auto at = [&](size_t i) -> char16_t { return i < length ? value[i] : 0; };
  size_t pos = 0;
  while (pos < length) {
    pos = FindIgnoringAsciiCase(value, kCharsetString, pos);
    if (pos == std::u16string_view::npos) {
      break;
    }
    pos += kCharsetString.size();
    while (pos < length && value[pos] <= ' ') {
      ++pos;
    }
    if (at(pos) != '=') {
      continue;
    }
    ++pos;
    while (pos < length && value[pos] <= ' ') {
      ++pos;
    }
    char16_t quote_mark = 0;
    if (pos < length && (value[pos] == '"' || value[pos] == '\'')) {
      quote_mark = value[pos++];
    }
    if (pos == length) {
      break;
    }
    size_t end = pos;
    while (end < length &&
           ((quote_mark && value[end] != quote_mark) ||
            (!quote_mark && value[end] > ' ' && value[end] != '"' &&
             value[end] != '\'' && value[end] != ';'))) {
      ++end;
    }
    // No closing quote.
    if (quote_mark && end == length) {
      break;
    }
    return value.substr(pos, end - pos);
  }
  return std::u16string_view();
}

}  // namespace mitto
