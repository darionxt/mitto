#include "mitto/dom/parsed_content_type.h"

#include <ranges>
#include <utility>

#include "base/check.h"
#include "base/strings/string_util.h"

namespace mitto {
namespace {

bool IsTokenCharacter(HeaderFieldMode mode, char16_t c) {
  if (c >= 128 || c < 0x20) {
    return false;
  }
  switch (c) {
    case ' ':
    case ';':
    case '"':
      return false;
    case '(':
    case ')':
    case '<':
    case '>':
    case '@':
    case ',':
    case ':':
    case '\\':
    case '/':
    case '[':
    case ']':
    case '?':
    case '=':
      return mode == HeaderFieldMode::kRelaxed;
    default:
      return true;
  }
}

// Blink's HeaderFieldTokenizer.
class HeaderFieldTokenizer {
 public:
  explicit HeaderFieldTokenizer(std::u16string_view input) : input_(input) {
    SkipOptionalWhitespace();
  }

  bool IsConsumed() const { return index_ >= input_.size(); }

  bool Consume(char16_t c) {
    DCHECK_NE(c, ' ');
    DCHECK_NE(c, '\t');
    if (IsConsumed() || input_[index_] != c) {
      return false;
    }
    ++index_;
    SkipOptionalWhitespace();
    return true;
  }

  bool ConsumeToken(HeaderFieldMode mode, std::u16string_view* output) {
    const size_t start = index_;
    while (!IsConsumed() && IsTokenCharacter(mode, input_[index_])) {
      ++index_;
    }
    if (start == index_) {
      return false;
    }
    *output = input_.substr(start, index_ - start);
    SkipOptionalWhitespace();
    return true;
  }

  bool ConsumeTokenOrQuotedString(HeaderFieldMode mode,
                                  std::u16string* output) {
    if (IsConsumed()) {
      return false;
    }
    if (input_[index_] == '"') {
      return ConsumeQuotedString(output);
    }
    std::u16string_view token;
    if (!ConsumeToken(mode, &token)) {
      return false;
    }
    *output = std::u16string(token);
    return true;
  }

 private:
  bool ConsumeQuotedString(std::u16string* output) {
    std::u16string builder;
    DCHECK_EQ('"', input_[index_]);
    ++index_;
    while (!IsConsumed()) {
      if (input_[index_] == '"') {
        *output = std::move(builder);
        ++index_;
        SkipOptionalWhitespace();
        return true;
      }
      if (input_[index_] == '\\') {
        ++index_;
        if (IsConsumed()) {
          return false;
        }
      }
      builder.push_back(input_[index_]);
      ++index_;
    }
    return false;
  }

  void SkipOptionalWhitespace() {
    while (!IsConsumed() && (input_[index_] == ' ' || input_[index_] == '\t')) {
      ++index_;
    }
  }

  size_t index_ = 0;
  const std::u16string_view input_;
};

// parameters := *(";" parameter), each attribute "=" token / quoted-string.
std::optional<ParsedContentParameters> ParseParameters(
    HeaderFieldTokenizer& tokenizer,
    HeaderFieldMode mode) {
  std::vector<std::pair<std::u16string, std::u16string>> parameters;
  while (!tokenizer.IsConsumed()) {
    if (!tokenizer.Consume(';')) {
      return std::nullopt;
    }
    std::u16string_view key;
    std::u16string value;
    if (!tokenizer.ConsumeToken(HeaderFieldMode::kNormal, &key) ||
        !tokenizer.Consume('=') ||
        !tokenizer.ConsumeTokenOrQuotedString(mode, &value)) {
      return std::nullopt;
    }
    parameters.emplace_back(std::u16string(key), std::move(value));
  }
  return ParsedContentParameters(std::move(parameters));
}

}  // namespace

ParsedContentParameters::ParsedContentParameters(
    std::vector<std::pair<std::u16string, std::u16string>> parameters)
    : parameters_(std::move(parameters)) {}
ParsedContentParameters::ParsedContentParameters(
    const ParsedContentParameters&) = default;
ParsedContentParameters& ParsedContentParameters::operator=(
    const ParsedContentParameters&) = default;
ParsedContentParameters::~ParsedContentParameters() = default;

std::optional<std::u16string> ParsedContentParameters::ValueForName(
    std::u16string_view name) const {
  if (!base::IsStringASCII(name)) {
    return std::nullopt;
  }
  for (const auto& [parameter, value] : std::views::reverse(parameters_)) {
    if (base::EqualsCaseInsensitiveASCII(parameter, name)) {
      return value;
    }
  }
  return std::nullopt;
}

ParsedContentType::ParsedContentType(std::u16string_view content_type,
                                     HeaderFieldMode mode) {
  HeaderFieldTokenizer tokenizer(content_type);
  std::u16string_view type;
  std::u16string_view subtype;
  if (!tokenizer.ConsumeToken(HeaderFieldMode::kNormal, &type) ||
      !tokenizer.Consume('/') ||
      !tokenizer.ConsumeToken(HeaderFieldMode::kNormal, &subtype)) {
    return;
  }
  mime_type_ = std::u16string(type) + u"/" + std::u16string(subtype);
  parameters_ = ParseParameters(tokenizer, mode);
}

ParsedContentType::~ParsedContentType() = default;

std::optional<std::u16string> ParsedContentType::ParameterValueForName(
    std::u16string_view name) const {
  return parameters_ ? parameters_->ValueForName(name) : std::nullopt;
}

ParsedContentDisposition::ParsedContentDisposition(
    std::u16string_view content_disposition,
    HeaderFieldMode mode) {
  HeaderFieldTokenizer tokenizer(content_disposition);
  std::u16string_view type;
  if (!tokenizer.ConsumeToken(HeaderFieldMode::kNormal, &type)) {
    return;
  }
  type_ = std::u16string(type);
  parameters_ = ParseParameters(tokenizer, mode);
}

ParsedContentDisposition::~ParsedContentDisposition() = default;

std::optional<std::u16string> ParsedContentDisposition::ParameterValueForName(
    std::u16string_view name) const {
  return parameters_ ? parameters_->ValueForName(name) : std::nullopt;
}

}  // namespace mitto
