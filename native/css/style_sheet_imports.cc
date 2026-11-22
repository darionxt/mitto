#include "mitto/css/style_sheet_imports.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "mitto/css/css_token.h"
#include "mitto/css/css_token_stream.h"

namespace mitto {
namespace {

// CSSParserImpl::AllowedRulesType at the top level, in order.
enum class AllowedRules {
  kAllowCharsetRules,
  kAllowLayerStatementRules,
  kAllowImportRules,
  kAllowNamespaceRules,
  kRegularRules,
};

// What a rule at the top level is, of those that change what may follow.
enum class RuleKind {
  // A rule that does not parse, which the parser drops.
  kInvalid,
  kCharset,
  kLayerStatement,
  kImport,
  kNamespace,
  kRegular,
};

// ComputeNewAllowedRules().
AllowedRules ComputeNewAllowedRules(AllowedRules allowed, RuleKind rule) {
  switch (rule) {
    case RuleKind::kInvalid:
      return allowed;
    case RuleKind::kCharset:
      return AllowedRules::kAllowLayerStatementRules;
    case RuleKind::kLayerStatement:
      return allowed <= AllowedRules::kAllowLayerStatementRules
                 ? AllowedRules::kAllowLayerStatementRules
                 : AllowedRules::kRegularRules;
    case RuleKind::kImport:
      return AllowedRules::kAllowImportRules;
    case RuleKind::kNamespace:
      return AllowedRules::kAllowNamespaceRules;
    case RuleKind::kRegular:
      return AllowedRules::kRegularRules;
  }
}

// The at-rules that Blink parses at the top level of a style sheet, with a
// block or as a statement, which a valid one of makes the rest regular.
bool IsKnownBlockAtRule(std::u16string_view name) {
  static constexpr std::array<std::u16string_view, 17> kNames = {
      u"media",
      u"supports",
      u"font-face",
      u"keyframes",
      u"-webkit-keyframes",
      u"page",
      u"counter-style",
      u"font-palette-values",
      u"property",
      u"container",
      u"layer",
      u"scope",
      u"starting-style",
      u"view-transition",
      u"position-try",
      u"font-feature-values",
      u"function",
  };
  for (const std::u16string_view known : kNames) {
    if (name == known) {
      return true;
    }
  }
  return false;
}

bool IsWhitespace(const CssToken& token) {
  return token.type == CssTokenType::kWhitespace;
}

// Of a block's text from just after its start, the text up to its end, as
// the stream has consumed the block: without its closing character, which an
// end of the sheet may leave out.
std::u16string_view BlockContents(std::u16string_view text,
                                  size_t start,
                                  size_t end) {
  std::u16string_view contents = text.substr(start, end - start);
  if (!contents.empty() && (contents.back() == ')' || contents.back() == ']' ||
                            contents.back() == '}')) {
    contents.remove_suffix(1);
  }
  return contents;
}

// The rest of the prelude of an at-rule, up to its ; or block, and whether a
// block follows, which it consumes. Returns false at the end of the stream
// after a prelude with neither, which still ends a statement.
struct Prelude {
  std::vector<CssToken> tokens;
  // The string of each url() among `tokens`, by index, that holds nothing
  // else, as ConsumeUrlAsStringView() takes it.
  std::map<size_t, std::u16string> url_strings;
  // The text inside each block among `tokens`, by index.
  std::map<size_t, std::u16string_view> block_contents;
  bool has_block = false;
  // The text inside the rule's block.
  std::u16string_view body;
};

Prelude ConsumePrelude(CssTokenStream& stream, std::u16string_view text) {
  Prelude prelude;
  while (!stream.AtEnd()) {
    const CssToken& token = stream.Peek();
    if (token.type == CssTokenType::kSemicolon) {
      stream.Consume();
      return prelude;
    }
    if (token.type == CssTokenType::kLeftBrace) {
      prelude.has_block = true;
      size_t start = 0;
      {
        const CssTokenStream::BlockGuard guard(stream);
        start = stream.Offset();
      }
      prelude.body = BlockContents(text, start, stream.Offset());
      return prelude;
    }
    if (token.block_type == CssBlockType::kBlockStart) {
      // A block within the prelude, such as supports(...), stays one token
      // here, with its text, but for the string of a url(): white space, the
      // string, white space and the block's end.
      prelude.tokens.push_back(token);
      const size_t index = prelude.tokens.size() - 1;
      const CssToken& block = prelude.tokens.back();
      size_t start = 0;
      {
        const CssTokenStream::BlockGuard guard(stream);
        start = stream.Offset();
        if (block.type == CssTokenType::kFunction &&
            base::EqualsCaseInsensitiveASCII(block.value, u"url")) {
          stream.ConsumeWhitespace();
          if (!stream.AtEnd() && stream.Peek().type == CssTokenType::kString) {
            CssToken string = stream.ConsumeIncludingWhitespace();
            if (stream.AtEnd()) {
              prelude.url_strings[index] = std::move(string.value);
            }
          }
        }
      }
      prelude.block_contents[index] =
          BlockContents(text, start, stream.Offset());
      continue;
    }
    prelude.tokens.push_back(stream.Consume());
  }
  return prelude;
}

// The URL that a prelude of @import starts with, as
// CSSParserImpl::ConsumeImportRule() reads it: a URL, or url() or a string.
bool ParseImportPrelude(const Prelude& prelude, StyleSheetImport& import) {
  size_t i = 0;
  const std::vector<CssToken>& tokens = prelude.tokens;
  while (i < tokens.size() && IsWhitespace(tokens[i])) {
    ++i;
  }
  if (i >= tokens.size()) {
    return false;
  }
  const CssToken& url = tokens[i];
  if (url.type == CssTokenType::kUrl || url.type == CssTokenType::kString) {
    import.url = url.value;
  } else if (url.type == CssTokenType::kFunction &&
             base::EqualsCaseInsensitiveASCII(url.value, u"url")) {
    // url("...") holding a string alone, decoded as the tokenizer reads it.
    const auto string = prelude.url_strings.find(i);
    if (string == prelude.url_strings.end()) {
      return false;
    }
    import.url = string->second;
  } else {
    return false;
  }
  return true;
}

// A prelude of @namespace: an optional prefix, then a URL or a string.
bool IsNamespacePrelude(const Prelude& prelude) {
  std::vector<const CssToken*> parts;
  for (const CssToken& token : prelude.tokens) {
    if (!IsWhitespace(token)) {
      parts.push_back(&token);
    }
  }
  auto is_uri = [](const CssToken& token) {
    return token.type == CssTokenType::kUrl ||
           token.type == CssTokenType::kString ||
           (token.type == CssTokenType::kFunction &&
            base::EqualsCaseInsensitiveASCII(token.value, u"url"));
  };
  if (parts.size() == 1) {
    return is_uri(*parts[0]);
  }
  return parts.size() == 2 && parts[0]->type == CssTokenType::kIdent &&
         is_uri(*parts[1]);
}

// A prelude of a statement @layer: names apart by commas.
bool IsLayerStatementPrelude(const Prelude& prelude) {
  bool expect_name = true;
  bool any = false;
  for (const CssToken& token : prelude.tokens) {
    if (IsWhitespace(token)) {
      continue;
    }
    if (expect_name) {
      if (token.type != CssTokenType::kIdent) {
        return false;
      }
      any = true;
      expect_name = false;
    } else if (token.type == CssTokenType::kComma) {
      expect_name = true;
    } else if (!(token.type == CssTokenType::kDelimiter &&
                 token.delimiter == '.')) {
      return false;
    } else {
      expect_name = true;
    }
  }
  return any && !expect_name;
}

bool IsIdentNamed(const CssToken& token, std::u16string_view name) {
  return token.type == CssTokenType::kIdent &&
         base::EqualsCaseInsensitiveASCII(token.value, name);
}

bool IsIdentAmong(const CssToken& token,
                  std::initializer_list<std::u16string_view> names) {
  for (const std::u16string_view name : names) {
    if (IsIdentNamed(token, name)) {
      return true;
    }
  }
  return false;
}

// css_parsing_utils::IsCSSWideKeyword(), and "default", which no name may be
// either.
bool IsCssWideKeywordOrDefault(const CssToken& token) {
  return IsIdentAmong(token, {u"initial", u"inherit", u"unset", u"revert",
                              u"revert-layer", u"default"});
}

// css_parsing_utils::IsDashedIdent().
bool IsDashedIdent(const CssToken& token) {
  return token.type == CssTokenType::kIdent &&
         base::StartsWith(token.value, u"--");
}

// The prelude's tokens but white space, with each one's index.
std::vector<size_t> Parts(const Prelude& prelude) {
  std::vector<size_t> parts;
  for (size_t i = 0; i < prelude.tokens.size(); ++i) {
    if (!IsWhitespace(prelude.tokens[i])) {
      parts.push_back(i);
    }
  }
  return parts;
}

// <supports-in-parens> and the container query's <query-in-parens>: a block
// of ( or a function, which Blink takes, as <general-enclosed> at least,
// whatever it holds.
bool IsInParens(const CssToken& token) {
  return token.type == CssTokenType::kLeftParenthesis ||
         token.type == CssTokenType::kFunction;
}

// CSSSupportsParser::ConsumeSupportsCondition() and ContainerQueryParser's
// condition, as far as whether one parses: not and one in parens, or ones in
// parens joined by and alone or or alone, to the end.
bool IsCondition(const Prelude& prelude,
                 const std::vector<size_t>& parts,
                 size_t first) {
  if (first >= parts.size()) {
    return false;
  }
  const std::vector<CssToken>& tokens = prelude.tokens;
  if (IsIdentNamed(tokens[parts[first]], u"not")) {
    return parts.size() == first + 2 && IsInParens(tokens[parts[first + 1]]);
  }
  if (!IsInParens(tokens[parts[first]])) {
    return false;
  }
  std::u16string_view joiner;
  for (size_t i = first + 1; i < parts.size(); i += 2) {
    const CssToken& token = tokens[parts[i]];
    const bool is_and = IsIdentNamed(token, u"and");
    if (!is_and && !IsIdentNamed(token, u"or")) {
      return false;
    }
    const std::u16string_view this_joiner = is_and ? u"and" : u"or";
    if (!joiner.empty() && joiner != this_joiner) {
      return false;
    }
    joiner = this_joiner;
    if (i + 1 >= parts.size() || !IsInParens(tokens[parts[i + 1]])) {
      return false;
    }
  }
  return true;
}

// ConsumeContainerRule(): a name that is no keyword, a condition, or both.
bool IsContainerPrelude(const Prelude& prelude) {
  const std::vector<size_t> parts = Parts(prelude);
  if (parts.empty()) {
    return false;
  }
  const CssToken& first = prelude.tokens[parts[0]];
  if (first.type != CssTokenType::kIdent || IsIdentNamed(first, u"not")) {
    return IsCondition(prelude, parts, 0);
  }
  if (IsIdentAmong(first, {u"none", u"and", u"or"}) ||
      IsCssWideKeywordOrDefault(first)) {
    return false;
  }
  return parts.size() == 1 || IsCondition(prelude, parts, 1);
}

// ConsumeLayerRule() with a block: none, or one name of idents joined by
// dots, with nothing between them.
bool IsLayerBlockPrelude(const Prelude& prelude) {
  size_t begin = 0;
  size_t end = prelude.tokens.size();
  while (begin < end && IsWhitespace(prelude.tokens[begin])) {
    ++begin;
  }
  while (end > begin && IsWhitespace(prelude.tokens[end - 1])) {
    --end;
  }
  if (begin == end) {
    return true;
  }
  for (size_t i = begin; i < end; ++i) {
    const CssToken& token = prelude.tokens[i];
    const bool want_name = (i - begin) % 2 == 0;
    if (want_name ? token.type != CssTokenType::kIdent
                  : !(token.type == CssTokenType::kDelimiter &&
                      token.delimiter == '.')) {
      return false;
    }
  }
  return (end - begin) % 2 == 1;
}

// ParsePageSelector(): none, or a name, a pseudo-class of :first, :left or
// :right, or both, with nothing between them.
bool IsPagePrelude(const Prelude& prelude) {
  size_t begin = 0;
  size_t end = prelude.tokens.size();
  while (begin < end && IsWhitespace(prelude.tokens[begin])) {
    ++begin;
  }
  while (end > begin && IsWhitespace(prelude.tokens[end - 1])) {
    --end;
  }
  size_t i = begin;
  if (i < end && prelude.tokens[i].type == CssTokenType::kIdent) {
    ++i;
  }
  if (i < end) {
    if (prelude.tokens[i].type != CssTokenType::kColon || i + 1 >= end ||
        !IsIdentAmong(prelude.tokens[i + 1], {u"first", u"left", u"right"})) {
      return false;
    }
    i += 2;
  }
  return i == end;
}

// ConsumeScopeRule(): none, ( a selector list ), then to and one, or to and
// one alone.
bool IsScopePrelude(const Prelude& prelude,
                    const CssSelectorParserContext& context) {
  const std::vector<size_t> parts = Parts(prelude);
  auto is_selectors = [&](size_t part) {
    const size_t index = parts[part];
    if (prelude.tokens[index].type != CssTokenType::kLeftParenthesis) {
      return false;
    }
    const auto contents = prelude.block_contents.find(index);
    return contents != prelude.block_contents.end() &&
           ParseCssSelectorList(contents->second, context) != nullptr;
  };
  size_t i = 0;
  if (i < parts.size() && !IsIdentNamed(prelude.tokens[parts[i]], u"to")) {
    if (!is_selectors(i)) {
      return false;
    }
    ++i;
  }
  if (i < parts.size()) {
    if (!IsIdentNamed(prelude.tokens[parts[i]], u"to") ||
        i + 1 >= parts.size() || !is_selectors(i + 1)) {
      return false;
    }
    i += 2;
  }
  return i == parts.size();
}

// css_parsing_utils::ConsumeFontFamily() of names alone: a string or idents
// for each, apart by commas, but for a generic family or a keyword that is
// a name alone.
bool IsFontFamilyNames(const Prelude& prelude) {
  const std::vector<size_t> parts = Parts(prelude);
  size_t i = 0;
  while (true) {
    if (i >= parts.size()) {
      return false;
    }
    const CssToken& first = prelude.tokens[parts[i]];
    if (first.type == CssTokenType::kString) {
      ++i;
    } else if (first.type == CssTokenType::kIdent) {
      size_t count = 0;
      while (i < parts.size() &&
             prelude.tokens[parts[i]].type == CssTokenType::kIdent) {
        ++i;
        ++count;
      }
      if (count == 1 &&
          (IsCssWideKeywordOrDefault(first) ||
           IsIdentAmong(first, {u"serif", u"sans-serif", u"cursive", u"fantasy",
                                u"monospace", u"system-ui", u"math",
                                u"-webkit-body"}))) {
        return false;
      }
    } else {
      return false;
    }
    if (i == parts.size()) {
      return true;
    }
    if (prelude.tokens[parts[i]].type != CssTokenType::kComma) {
      return false;
    }
    ++i;
  }
}

// The tokens of `text`, white space included, with the text of each block,
// as a prelude holds them, but taking ; and { as tokens like any other, as
// the tokenizer reads them, escaped ones in an ident included. A block end
// with no start ends the stream early, and a token of ; then stands for the
// rest, which no syntax or parameter list takes.
Prelude TokensWithBlocks(std::u16string_view text) {
  Prelude prelude;
  CssTokenStream stream(text);
  while (!stream.AtEnd()) {
    if (stream.Peek().block_type == CssBlockType::kBlockStart) {
      prelude.tokens.push_back(stream.Peek());
      const size_t index = prelude.tokens.size() - 1;
      size_t start = 0;
      {
        const CssTokenStream::BlockGuard guard(stream);
        start = stream.Offset();
      }
      prelude.block_contents[index] =
          BlockContents(text, start, stream.Offset());
      continue;
    }
    prelude.tokens.push_back(stream.Consume());
  }
  if (stream.Peek().type != CssTokenType::kEndOfFile) {
    CssToken rest;
    rest.type = CssTokenType::kSemicolon;
    prelude.tokens.push_back(std::move(rest));
  }
  return prelude;
}

void SkipWhitespace(const Prelude& prelude, size_t& i) {
  while (i < prelude.tokens.size() && IsWhitespace(prelude.tokens[i])) {
    ++i;
  }
}

bool IsDelimiter(const Prelude& prelude, size_t i, char32_t delimiter) {
  return i < prelude.tokens.size() &&
         prelude.tokens[i].type == CssTokenType::kDelimiter &&
         prelude.tokens[i].delimiter == delimiter;
}

// ConsumeTypeName().
bool IsSyntaxTypeName(std::u16string_view name) {
  static constexpr std::array<std::u16string_view, 15> kTypes = {
      u"angle",
      u"color",
      u"custom-ident",
      u"image",
      u"integer",
      u"length",
      u"length-percentage",
      u"number",
      u"percentage",
      u"resolution",
      u"string",
      u"time",
      u"url",
      u"transform-function",
      u"transform-list"};
  return std::find(kTypes.begin(), kTypes.end(), name) != kTypes.end();
}

// ConsumeSyntaxComponent(): a type in angle brackets, or an ident that no
// keyword is, then + or #, but not after <transform-list>, and white space.
bool ConsumeSyntaxComponent(const Prelude& prelude, size_t& i) {
  const std::vector<CssToken>& tokens = prelude.tokens;
  size_t j = i;
  bool transform_list = false;
  if (IsDelimiter(prelude, j, '<')) {
    ++j;
    if (j >= tokens.size() || tokens[j].type != CssTokenType::kIdent ||
        !IsSyntaxTypeName(tokens[j].value)) {
      return false;
    }
    transform_list = tokens[j].value == u"transform-list";
    ++j;
    if (!IsDelimiter(prelude, j, '>')) {
      return false;
    }
    ++j;
  } else if (j < tokens.size() && tokens[j].type == CssTokenType::kIdent &&
             !IsCssWideKeywordOrDefault(tokens[j])) {
    ++j;
  } else {
    return false;
  }
  const bool repeated =
      IsDelimiter(prelude, j, '#') || IsDelimiter(prelude, j, '+');
  if (repeated) {
    ++j;
  }
  SkipWhitespace(prelude, j);
  if (transform_list && repeated) {
    return false;
  }
  i = j;
  return true;
}

// CSSSyntaxDefinition::Consume(): *, or components apart by |.
bool ConsumeSyntax(const Prelude& prelude, size_t& i) {
  if (IsDelimiter(prelude, i, '*')) {
    ++i;
    SkipWhitespace(prelude, i);
    return true;
  }
  size_t j = i;
  while (true) {
    if (!ConsumeSyntaxComponent(prelude, j)) {
      return false;
    }
    if (!IsDelimiter(prelude, j, '|')) {
      break;
    }
    ++j;
    SkipWhitespace(prelude, j);
  }
  i = j;
  return true;
}

// ConsumeFunctionType(): type() holding a whole syntax, or a lone component.
bool ConsumeFunctionType(const Prelude& prelude, size_t& i) {
  if (i < prelude.tokens.size() &&
      prelude.tokens[i].type == CssTokenType::kFunction &&
      base::EqualsCaseInsensitiveASCII(prelude.tokens[i].value, u"type")) {
    const auto contents = prelude.block_contents.find(i);
    if (contents != prelude.block_contents.end()) {
      const Prelude inner = TokensWithBlocks(contents->second);
      size_t k = 0;
      SkipWhitespace(inner, k);
      if (ConsumeSyntax(inner, k) && k == inner.tokens.size()) {
        ++i;
        SkipWhitespace(prelude, i);
        return true;
      }
    }
  }
  return ConsumeSyntaxComponent(prelude, i);
}

// ConsumeFunctionParameters(): none, or names of custom properties apart by
// commas, each with a type and a default after a colon if it has them.
// Whether the default fits the type needs the values of style.
bool IsFunctionParameters(std::u16string_view text) {
  const Prelude parameters = TokensWithBlocks(text);
  const std::vector<CssToken>& tokens = parameters.tokens;
  size_t i = 0;
  bool first = true;
  while (true) {
    SkipWhitespace(parameters, i);
    if (first && i == tokens.size()) {
      return true;
    }
    if (i >= tokens.size() || tokens[i].type != CssTokenType::kIdent ||
        tokens[i].value.size() < 3 ||
        !base::StartsWith(tokens[i].value, u"--")) {
      return false;
    }
    ++i;
    SkipWhitespace(parameters, i);
    if (size_t typed = i; ConsumeFunctionType(parameters, typed)) {
      i = typed;
    }
    if (i < tokens.size() && tokens[i].type == CssTokenType::kColon) {
      ++i;
      while (i < tokens.size() && tokens[i].type != CssTokenType::kComma) {
        ++i;
      }
    }
    if (i == tokens.size()) {
      return true;
    }
    if (tokens[i].type != CssTokenType::kComma) {
      return false;
    }
    ++i;
    first = false;
  }
}

// ConsumeFunctionRule(): a function token, its parameters, then nothing, or
// returns and a type, then nothing.
bool IsFunctionPrelude(const Prelude& prelude) {
  size_t i = 0;
  SkipWhitespace(prelude, i);
  if (i >= prelude.tokens.size() ||
      prelude.tokens[i].type != CssTokenType::kFunction) {
    return false;
  }
  const auto contents = prelude.block_contents.find(i);
  if (!IsFunctionParameters(contents != prelude.block_contents.end()
                                ? contents->second
                                : std::u16string_view())) {
    return false;
  }
  ++i;
  SkipWhitespace(prelude, i);
  if (i < prelude.tokens.size() &&
      IsIdentNamed(prelude.tokens[i], u"returns")) {
    ++i;
    SkipWhitespace(prelude, i);
    if (!ConsumeFunctionType(prelude, i)) {
      return false;
    }
  }
  SkipWhitespace(prelude, i);
  return i == prelude.tokens.size();
}

// CSSSyntaxStringParser: a syntax, as type() holds one, read with the
// tokenizer's escapes.
bool IsSyntaxString(std::u16string_view syntax) {
  const Prelude tokens = TokensWithBlocks(syntax);
  size_t i = 0;
  SkipWhitespace(tokens, i);
  return ConsumeSyntax(tokens, i) && i == tokens.tokens.size();
}

// ConsumePropertyRule(): a dashed ident, and a block whose syntax and
// inherits parse, with an initial value unless the syntax is *. Whether the
// initial value fits the syntax, and is computationally independent, needs
// the values of style: a Known Gap.
bool IsPropertyRule(const Prelude& prelude) {
  const std::vector<size_t> parts = Parts(prelude);
  if (parts.size() != 1 || !IsDashedIdent(prelude.tokens[parts[0]])) {
    return false;
  }
  std::optional<std::u16string> syntax;
  std::optional<bool> inherits;
  bool initial_value = false;
  CssTokenStream stream(prelude.body);
  while (!stream.AtEnd()) {
    stream.ConsumeWhitespace();
    if (stream.AtEnd()) {
      break;
    }
    std::u16string name;
    if (stream.Peek().type == CssTokenType::kIdent) {
      name = base::ToLowerASCII(stream.Consume().value);
    }
    stream.ConsumeWhitespace();
    const bool has_colon =
        !stream.AtEnd() && stream.Peek().type == CssTokenType::kColon;
    std::vector<CssToken> value;
    if (has_colon) {
      stream.Consume();
    }
    while (!stream.AtEnd() && stream.Peek().type != CssTokenType::kSemicolon) {
      if (stream.Peek().block_type == CssBlockType::kBlockStart) {
        value.push_back(stream.Peek());
        const CssTokenStream::BlockGuard guard(stream);
        continue;
      }
      CssToken token = stream.Consume();
      if (!IsWhitespace(token)) {
        value.push_back(std::move(token));
      }
    }
    if (!stream.AtEnd()) {
      stream.Consume();
    }
    if (!has_colon) {
      continue;
    }
    if (name == u"syntax" && value.size() == 1 &&
        value[0].type == CssTokenType::kString &&
        IsSyntaxString(value[0].value)) {
      syntax = value[0].value;
    } else if (name == u"inherits" && value.size() == 1 &&
               IsIdentAmong(value[0], {u"true", u"false"})) {
      inherits = IsIdentNamed(value[0], u"true");
    } else if (name == u"initial-value") {
      initial_value = true;
    }
  }
  if (!syntax || !inherits) {
    return false;
  }
  return initial_value ||
         base::TrimString(*syntax, u" \t\n\f\r", base::TRIM_ALL) == u"*";
}

// Whether Blink's CSSParserImpl makes a rule of an at-rule of a known name
// that has a block, from its prelude, and for @property its descriptors.
// One that it drops leaves the imports that may follow as they were.
bool IsValidBlockAtRule(std::u16string_view name,
                        const Prelude& prelude,
                        const CssSelectorParserContext& context) {
  const std::vector<size_t> parts = Parts(prelude);
  const std::vector<CssToken>& tokens = prelude.tokens;
  auto single = [&]() -> const CssToken* {
    return parts.size() == 1 ? &tokens[parts[0]] : nullptr;
  };
  if (name == u"media") {
    // MediaQueryParser makes not all of a query that does not parse.
    return true;
  }
  if (name == u"supports") {
    return IsCondition(prelude, parts, 0);
  }
  if (name == u"font-face" || name == u"starting-style" ||
      name == u"view-transition") {
    return parts.empty();
  }
  if (name == u"keyframes" || name == u"-webkit-keyframes") {
    // IsValidIdentAnimationName(), or a string.
    const CssToken* const token = single();
    return token && (token->type == CssTokenType::kString ||
                     (token->type == CssTokenType::kIdent &&
                      !IsCssWideKeywordOrDefault(*token) &&
                      !IsIdentNamed(*token, u"none")));
  }
  if (name == u"counter-style") {
    // ConsumeCounterStyleNameInPrelude(): no name of a counter style that may
    // not be redefined.
    const CssToken* const token = single();
    return token && token->type == CssTokenType::kIdent &&
           !IsCssWideKeywordOrDefault(*token) &&
           !IsIdentAmong(*token,
                         {u"none", u"decimal", u"disc", u"square", u"circle",
                          u"disclosure-open", u"disclosure-closed"});
  }
  if (name == u"font-palette-values" || name == u"position-try") {
    const CssToken* const token = single();
    return token && IsDashedIdent(*token);
  }
  if (name == u"property") {
    return IsPropertyRule(prelude);
  }
  if (name == u"page") {
    return IsPagePrelude(prelude);
  }
  if (name == u"container") {
    return IsContainerPrelude(prelude);
  }
  if (name == u"layer") {
    return IsLayerBlockPrelude(prelude);
  }
  if (name == u"scope") {
    return IsScopePrelude(prelude, context);
  }
  if (name == u"font-feature-values") {
    return IsFontFamilyNames(prelude);
  }
  if (name == u"function") {
    return IsFunctionPrelude(prelude);
  }
  return false;
}

}  // namespace

std::vector<StyleSheetImport> FindStyleSheetImports(
    std::u16string_view text,
    const CssSelectorParserContext& context) {
  std::vector<StyleSheetImport> imports;
  CssTokenStream stream(text);
  AllowedRules allowed = AllowedRules::kAllowCharsetRules;
  while (true) {
    while (!stream.AtEnd() && (IsWhitespace(stream.Peek()) ||
                               stream.Peek().type == CssTokenType::kCdo ||
                               stream.Peek().type == CssTokenType::kCdc)) {
      stream.Consume();
    }
    if (stream.AtEnd()) {
      break;
    }
    RuleKind rule = RuleKind::kInvalid;
    if (stream.Peek().type == CssTokenType::kAtKeyword) {
      const std::u16string name = base::ToLowerASCII(stream.Consume().value);
      const Prelude prelude = ConsumePrelude(stream, text);
      if (name == u"charset") {
        // Blink makes no rule of @charset; it only ends the section where
        // one may stand.
        rule = allowed == AllowedRules::kAllowCharsetRules && !prelude.has_block
                   ? RuleKind::kCharset
                   : RuleKind::kInvalid;
      } else if (name == u"import") {
        StyleSheetImport import;
        if (allowed <= AllowedRules::kAllowImportRules && !prelude.has_block &&
            ParseImportPrelude(prelude, import)) {
          imports.push_back(std::move(import));
          rule = RuleKind::kImport;
        }
      } else if (name == u"namespace") {
        if (allowed <= AllowedRules::kAllowNamespaceRules &&
            !prelude.has_block && IsNamespacePrelude(prelude)) {
          rule = RuleKind::kNamespace;
        }
      } else if (name == u"layer" && !prelude.has_block) {
        if (IsLayerStatementPrelude(prelude)) {
          rule = RuleKind::kLayerStatement;
        }
      } else if (IsKnownBlockAtRule(name) && prelude.has_block &&
                 IsValidBlockAtRule(name, prelude, context)) {
        rule = RuleKind::kRegular;
      }
    } else {
      // A qualified rule: a selector list and its block.
      const size_t start = stream.Peek().start;
      size_t end = start;
      bool has_block = false;
      while (!stream.AtEnd()) {
        const CssToken& token = stream.Peek();
        if (token.type == CssTokenType::kLeftBrace) {
          end = token.start;
          has_block = true;
          const CssTokenStream::BlockGuard guard(stream);
          break;
        }
        if (token.block_type == CssBlockType::kBlockStart) {
          const CssTokenStream::BlockGuard guard(stream);
          continue;
        }
        stream.Consume();
      }
      if (has_block &&
          ParseCssSelectorList(text.substr(start, end - start), context)) {
        rule = RuleKind::kRegular;
      }
    }
    allowed = ComputeNewAllowedRules(allowed, rule);
  }
  return imports;
}

}  // namespace mitto
