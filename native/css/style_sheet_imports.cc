#include "mitto/css/style_sheet_imports.h"

#include <array>

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
  static constexpr std::array<std::u16string_view, 16> kNames = {
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

// The rest of the prelude of an at-rule, up to its ; or block, and whether a
// block follows, which it consumes. Returns false at the end of the stream
// after a prelude with neither, which still ends a statement.
struct Prelude {
  std::vector<CssToken> tokens;
  bool has_block = false;
};

Prelude ConsumePrelude(CssTokenStream& stream) {
  Prelude prelude;
  while (!stream.AtEnd()) {
    const CssToken& token = stream.Peek();
    if (token.type == CssTokenType::kSemicolon) {
      stream.Consume();
      return prelude;
    }
    if (token.type == CssTokenType::kLeftBrace) {
      prelude.has_block = true;
      const CssTokenStream::BlockGuard guard(stream);
      return prelude;
    }
    if (token.block_type == CssBlockType::kBlockStart) {
      // A block within the prelude, such as supports(...), stays one token
      // here and is skipped whole.
      prelude.tokens.push_back(token);
      const CssTokenStream::BlockGuard guard(stream);
      continue;
    }
    prelude.tokens.push_back(stream.Consume());
  }
  return prelude;
}

// The URL that a prelude of @import starts with, as
// CSSParserImpl::ConsumeImportRule() reads it: a URL, or url() or a string.
bool ParseImportPrelude(std::u16string_view text,
                        const Prelude& prelude,
                        StyleSheetImport& import) {
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
    // url("...") with a string inside, whose contents the block skipped:
    // they stand between its start and end in the sheet.
    const std::u16string_view inside = base::TrimWhitespace(
        text.substr(url.end, std::u16string_view::npos), base::TRIM_LEADING);
    if (inside.empty() || (inside[0] != '"' && inside[0] != '\'')) {
      return false;
    }
    const char16_t quote = inside[0];
    const size_t close = inside.find(quote, 1);
    if (close == std::u16string_view::npos) {
      return false;
    }
    import.url = std::u16string(inside.substr(1, close - 1));
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
      const Prelude prelude = ConsumePrelude(stream);
      if (name == u"charset") {
        // Blink makes no rule of @charset; it only ends the section where
        // one may stand.
        rule = allowed == AllowedRules::kAllowCharsetRules && !prelude.has_block
                   ? RuleKind::kCharset
                   : RuleKind::kInvalid;
      } else if (name == u"import") {
        StyleSheetImport import;
        if (allowed <= AllowedRules::kAllowImportRules && !prelude.has_block &&
            ParseImportPrelude(text, prelude, import)) {
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
      } else if (IsKnownBlockAtRule(name) && prelude.has_block) {
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
