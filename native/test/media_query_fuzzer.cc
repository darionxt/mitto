// Feeds any input to the CSS tokenizer and the media query parser and
// evaluator, which read what a page's scripts give matchMedia().
//
// The first byte says how. Its low bits turn on, one each, the
// runtime-enabled features that decide which media features parse:
// forced-colors, inverted-colors, prefers-reduced-data, video-dynamic-range,
// navigation-controls, viewport segments, device-posture and Chrome's
// additional windowing controls. The second byte's high bit says whether the
// rest is UTF-16, little-endian and with any code units, rather than UTF-8,
// and its other bits pick the machine that the queries are evaluated for.
//
// The tokens must cover the input in order, with every block that starts
// either ended or still open at the end, and when every block is ended, the
// media query list that the parser writes back must parse to itself. With one
// still open it need not: Blink closes a condition's parentheses when it
// writes it back, so an open block inside one gains a ")" each time.

#include "mitto/css/media_query.h"

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/strings/string_view_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/css/css_token.h"
#include "mitto/css/css_tokenizer.h"
#include "mitto/css/media_query_evaluator.h"
#include "mitto/css/media_query_parser.h"
#include "mitto/css/media_values.h"

namespace {

using mitto::CssBlockType;
using mitto::CssToken;
using mitto::CssTokenType;

// Returns whether every block that starts ends.
bool CheckTokens(std::u16string_view input) {
  const std::vector<CssToken> tokens = mitto::TokenizeCss(input);
  CHECK(!tokens.empty());
  CHECK_EQ(tokens.back().type, CssTokenType::kEndOfFile);
  CHECK_EQ(tokens.back().end, input.size());
  size_t previous_end = 0;
  std::vector<CssTokenType> open_blocks;
  for (size_t i = 0; i < tokens.size(); ++i) {
    const CssToken& token = tokens[i];
    CHECK_LE(previous_end, token.start);
    CHECK_LE(token.start, token.end);
    CHECK_LE(token.end, input.size());
    CHECK(token.type != CssTokenType::kEndOfFile || i + 1 == tokens.size());
    previous_end = token.end;
    if (token.block_type == CssBlockType::kBlockStart) {
      open_blocks.push_back(token.type == CssTokenType::kFunction
                                ? CssTokenType::kLeftParenthesis
                                : token.type);
    } else if (token.block_type == CssBlockType::kBlockEnd) {
      CHECK(!open_blocks.empty());
      // Each end follows its start's type by one, as in Blink.
      CHECK_EQ(static_cast<int>(open_blocks.back()) + 1,
               static_cast<int>(token.type));
      open_blocks.pop_back();
    }
  }
  return open_blocks.empty();
}

mitto::MediaValues Machine(uint8_t which) {
  mitto::MediaValues values;
  values.media_type = (which & 1) ? "print" : "screen";
  values.viewport_width = 1536 - (which & 0x0E) * 37.5;
  values.viewport_height = 778.5;
  values.device_width = 1536;
  values.device_height = 864;
  values.device_pixel_ratio = (which & 0x10) ? 1.25f : 1.0f;
  values.color_bits_per_component = (which & 0x20) ? 10 : 8;
  values.primary_pointer_type = mitto::kPointerTypeFine;
  values.available_pointer_types =
      mitto::kPointerTypeFine |
      ((which & 0x40) ? mitto::kPointerTypeCoarse : 0);
  values.primary_hover_type = mitto::kHoverTypeHover;
  values.available_hover_types = mitto::kHoverTypeHover;
  values.three_d_enabled = true;
  values.strict_mode = (which & 0x02) != 0;
  values.forced_colors = (which & 0x04) != 0;
  values.preferred_color_scheme = (which & 0x08)
                                      ? mitto::MediaColorScheme::kDark
                                      : mitto::MediaColorScheme::kLight;
  values.scripting = mitto::MediaScripting::kEnabled;
  values.horizontal_viewport_segments = 1;
  values.vertical_viewport_segments = 1;
  return values;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // SAFETY: libFuzzer says `data` holds `size` bytes.
  base::span<const uint8_t> bytes = UNSAFE_BUFFERS(base::span(data, size));
  if (bytes.size() < 2) {
    return 0;
  }
  const uint8_t feature_bits = bytes.take_first_elem();
  const uint8_t how = bytes.take_first_elem();
  mitto::MediaQueryFeatures features;
  features.forced_colors = (feature_bits & 0x01) != 0;
  features.inverted_colors = (feature_bits & 0x02) != 0;
  features.prefers_reduced_data = (feature_bits & 0x04) != 0;
  features.video_dynamic_range = (feature_bits & 0x08) != 0;
  features.navigation_controls = (feature_bits & 0x10) != 0;
  features.viewport_segments = (feature_bits & 0x20) != 0;
  features.device_posture = (feature_bits & 0x40) != 0;
  features.additional_windowing_controls = (feature_bits & 0x80) != 0;

  std::u16string input;
  if (how & 0x80) {
    while (bytes.size() >= 2) {
      const auto unit = bytes.take_first<2u>();
      input.push_back(static_cast<char16_t>(unit[0] | (unit[1] << 8)));
    }
  } else {
    input = base::UTF8ToUTF16(base::as_string_view(bytes));
  }

  const bool blocks_end = CheckTokens(input);

  const std::unique_ptr<mitto::MediaQuerySet> query_set =
      mitto::ParseMediaQuerySet(input, features);
  const std::u16string media_text = query_set->MediaText();
  const std::unique_ptr<mitto::MediaQuerySet> again =
      mitto::ParseMediaQuerySet(media_text, features);
  if (blocks_end) {
    CHECK(again->MediaText() == media_text);
  }

  // Six digits of a number written back can evaluate otherwise than the
  // number did, so only the evaluation itself is checked.
  const mitto::MediaValues values = Machine(how & 0x7F);
  const mitto::MediaQueryEvaluator evaluator(values);
  evaluator.Eval(*query_set);
  evaluator.Eval(*again);
  return 0;
}
