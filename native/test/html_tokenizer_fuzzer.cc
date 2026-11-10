// Feeds any input to the HTML tokenizer, which reads what a page's server
// sends and what its scripts write.
//
// The first two bytes say how. In the first, the low three bits are the
// state to start in, the next three how big the pieces are that the input
// comes in, the next whether to switch states after start tags as a tree
// builder does, and the high bit whether the rest is UTF-16, little-endian
// and with any code units, rather than UTF-8. In the second, the low three
// bits turn on forced U+0000 replacement and CDATA sections, and turn off
// processing instructions. The input must give the same tokens and parse
// errors whole as in pieces.

#include "mitto/html/html_tokenizer.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/notreached.h"
#include "base/strings/string_view_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/html/html_token.h"

namespace {

using mitto::HtmlToken;
using mitto::HtmlTokenizer;
using State = HtmlTokenizer::State;

constexpr auto kStates = std::to_array<State>({
    State::kData,
    State::kRcdata,
    State::kRawtext,
    State::kScriptData,
    State::kPlaintext,
    State::kCdataSection,
    State::kData,
    State::kData,
});

constexpr auto kPieceSizes =
    std::to_array<size_t>({1, 2, 3, 5, 8, 13, 64, 4096});

struct Settings {
  State state;
  bool tree_builder;
  bool force_null_character_replacement;
  bool allow_cdata;
  bool processing_instructions;
};

struct Output {
  std::vector<HtmlToken> tokens;
  std::vector<HtmlTokenizer::ParseError> errors;
};

// The state Blink's tree builder switches to after a start tag in the body.
std::optional<State> StateAfter(std::u16string_view tag_name) {
  if (tag_name == u"title" || tag_name == u"textarea") {
    return State::kRcdata;
  }
  if (tag_name == u"style" || tag_name == u"xmp" || tag_name == u"iframe" ||
      tag_name == u"noembed" || tag_name == u"noframes") {
    return State::kRawtext;
  }
  if (tag_name == u"script") {
    return State::kScriptData;
  }
  if (tag_name == u"plaintext") {
    return State::kPlaintext;
  }
  return std::nullopt;
}

Output Tokenize(std::u16string_view input,
                const Settings& settings,
                size_t piece_size) {
  HtmlTokenizer tokenizer(
      {.processing_instructions = settings.processing_instructions,
       .collect_parse_errors = true});
  tokenizer.SetState(settings.state);
  tokenizer.SetForceNullCharacterReplacement(
      settings.force_null_character_replacement);
  tokenizer.SetAllowCdata(settings.allow_cdata);

  Output output;
  bool end_of_file = false;
  const auto take_tokens = [&] {
    HtmlToken token;
    while (tokenizer.NextToken(token)) {
      CHECK(!end_of_file);
      switch (token.type) {
        case HtmlToken::Type::kCharacter:
          CHECK(!token.data.empty());
          // Where the input is cut decides how characters split into tokens.
          if (!output.tokens.empty() &&
              output.tokens.back().type == HtmlToken::Type::kCharacter) {
            output.tokens.back().data += token.data;
            continue;
          }
          break;
        case HtmlToken::Type::kStartTag: {
          CHECK(!token.name.empty());
          std::set<std::u16string> names;
          for (const mitto::HtmlAttribute& attribute : token.attributes) {
            CHECK(names.insert(attribute.name).second);
          }
          if (settings.tree_builder) {
            if (const std::optional<State> state = StateAfter(token.name)) {
              tokenizer.SetState(*state);
            }
          }
          break;
        }
        case HtmlToken::Type::kEndTag:
          CHECK(!token.name.empty());
          break;
        case HtmlToken::Type::kEndOfFile:
          end_of_file = true;
          break;
        case HtmlToken::Type::kUninitialized:
          NOTREACHED();
        default:
          break;
      }
      output.tokens.push_back(std::move(token));
    }
  };
  while (!input.empty()) {
    const size_t size = std::min(piece_size, input.size());
    tokenizer.Append(input.substr(0, size));
    input.remove_prefix(size);
    take_tokens();
  }
  CHECK(!end_of_file);
  tokenizer.Finish();
  take_tokens();
  CHECK(end_of_file);

  output.errors = tokenizer.TakeParseErrors();
  for (const HtmlTokenizer::ParseError& error : output.errors) {
    CHECK_GE(error.line, 1);
    CHECK_GE(error.column, 1);
  }
  return output;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // SAFETY: libFuzzer says `data` holds `size` bytes.
  base::span<const uint8_t> bytes = UNSAFE_BUFFERS(base::span(data, size));
  if (bytes.size() < 2) {
    return 0;
  }
  const uint8_t how = bytes.take_first_elem();
  const uint8_t flags = bytes.take_first_elem();
  const Settings settings = {
      .state = kStates[how & 7],
      .tree_builder = (how & 0x40) != 0,
      .force_null_character_replacement = (flags & 1) != 0,
      .allow_cdata = (flags & 2) != 0,
      .processing_instructions = (flags & 4) == 0,
  };
  const size_t piece_size = kPieceSizes[(how >> 3) & 7];

  std::u16string input;
  if (how & 0x80) {
    while (bytes.size() >= 2) {
      const auto unit = bytes.take_first<2u>();
      input.push_back(static_cast<char16_t>(unit[0] | (unit[1] << 8)));
    }
  } else {
    input = base::UTF8ToUTF16(base::as_string_view(bytes));
  }

  const Output whole = Tokenize(input, settings, input.size() + 1);
  const Output pieces = Tokenize(input, settings, piece_size);
  CHECK(pieces.tokens == whole.tokens);
  CHECK(pieces.errors == whole.errors);
  return 0;
}
