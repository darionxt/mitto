// A token of the HTML Standard's tokenizer, which the tree builder consumes:
// https://html.spec.whatwg.org/multipage/parsing.html#tokenization
// It holds what Blink's HTMLToken holds, processing instructions included.
//
// Upstream: third_party/blink/renderer/core/html/parser/html_token.h

#ifndef MITTO_HTML_HTML_TOKEN_H_
#define MITTO_HTML_HTML_TOKEN_H_

#include <optional>
#include <string>
#include <vector>

namespace mitto {

struct HtmlAttribute {
  friend bool operator==(const HtmlAttribute&, const HtmlAttribute&) = default;

  std::u16string name;
  std::u16string value;
};

struct HtmlToken {
  enum class Type {
    kUninitialized,
    kDoctype,
    kStartTag,
    kEndTag,
    kComment,
    kCharacter,
    // <?target data>.
    kProcessingInstruction,
    kEndOfFile,
  };

  HtmlToken();
  HtmlToken(const HtmlToken&);
  HtmlToken(HtmlToken&&);
  HtmlToken& operator=(const HtmlToken&);
  HtmlToken& operator=(HtmlToken&&);
  ~HtmlToken();

  friend bool operator==(const HtmlToken&, const HtmlToken&) = default;

  Type type = Type::kUninitialized;

  // A tag's name, a DOCTYPE's name or a processing instruction's target.
  std::u16string name;
  // Whether a DOCTYPE has no name at all, as <!DOCTYPE> has not.
  bool name_missing = false;

  // A comment's, a processing instruction's or a run of characters' data.
  std::u16string data;

  // A tag's attributes, in the order of the input. A start tag has no two of
  // the same name: the tokenizer keeps the first.
  std::vector<HtmlAttribute> attributes;
  bool self_closing = false;

  // A DOCTYPE's identifiers, missing unless it gives them, and its
  // force-quirks flag.
  std::optional<std::u16string> public_identifier;
  std::optional<std::u16string> system_identifier;
  bool force_quirks = false;
};

}  // namespace mitto

#endif  // MITTO_HTML_HTML_TOKEN_H_
