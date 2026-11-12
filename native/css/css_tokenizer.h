// CSS Syntax's tokenizer, as Blink's CSSTokenizer runs it:
// https://drafts.csswg.org/css-syntax/#tokenization
//
// It reads UTF-16 code units, as Blink does, and replaces U+0000 and lone
// surrogates with U+FFFD where CSS Syntax's preprocessing would, without a
// pass of its own. Comments are skipped. Blink tokenizes as its parser asks;
// this tokenizes the whole input at once, which gives the same tokens, since
// what comes next never depends on the parser, and marks block starts and the
// ends that close them as Blink does, so that a parser can find where each
// block ends. M9's CSS parser takes this over.
//
// Upstream: third_party/blink/renderer/core/css/parser/css_tokenizer.cc
// Upstream: third_party/blink/renderer/core/css/parser/css_tokenizer_input_stream.h
// Upstream: third_party/blink/renderer/core/css/parser/css_parser_idioms.cc

#ifndef MITTO_CSS_CSS_TOKENIZER_H_
#define MITTO_CSS_CSS_TOKENIZER_H_

#include <string_view>
#include <vector>

#include "mitto/css/css_token.h"

namespace mitto {

// Every token of `input`, ending with one of type kEndOfFile.
std::vector<CssToken> TokenizeCss(std::u16string_view input);

}  // namespace mitto

#endif  // MITTO_CSS_CSS_TOKENIZER_H_
