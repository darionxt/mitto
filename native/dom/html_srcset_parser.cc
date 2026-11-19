#include "mitto/dom/html_srcset_parser.h"

#include <stddef.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/dom/html_parser_idioms.h"

namespace mitto {
namespace {

bool IsComma(char16_t c) {
  return c == ',';
}

bool IsHTMLSpaceOrComma(char16_t c) {
  return IsHTMLSpace(c) || IsComma(c);
}

bool IsAsciiDigit(char16_t c) {
  return c >= '0' && c <= '9';
}

// A descriptor: where it is in the attribute, and how long.
struct DescriptorToken {
  size_t start;
  size_t length;

  size_t LastIndex() const { return start + length - 1; }

  // A valid non-negative integer before the descriptor's last character.
  std::optional<int> ToInt(std::u16string_view attribute) const {
    const std::u16string_view digits = attribute.substr(start, length - 1);
    if (!std::ranges::all_of(digits, IsAsciiDigit)) {
      return std::nullopt;
    }
    int value;
    if (!base::StringToInt(digits, &value)) {
      return std::nullopt;
    }
    return value;
  }

  // ParseToDecimalForNumberType(): a valid floating-point number before the
  // descriptor's last character, without a leading "+", finite in a double.
  std::optional<float> ToFloat(std::u16string_view attribute) const {
    const std::u16string_view number = attribute.substr(start, length - 1);
    if (number.empty() || number.ends_with('.')) {
      return std::nullopt;
    }
    if (number[0] != '-' && number[0] != '.' && !IsAsciiDigit(number[0])) {
      return std::nullopt;
    }
    // Decimal::FromString(): a sign, digits with at most one full stop
    // between or before them, and an exponent of digits with a sign.
    size_t position = number[0] == '-' ? 1 : 0;
    size_t integer_digits = 0;
    while (position < number.size() && IsAsciiDigit(number[position])) {
      ++position;
      ++integer_digits;
    }
    size_t fraction_digits = 0;
    if (position < number.size() && number[position] == '.') {
      ++position;
      while (position < number.size() && IsAsciiDigit(number[position])) {
        ++position;
        ++fraction_digits;
      }
    }
    if (!integer_digits && !fraction_digits) {
      return std::nullopt;
    }
    if (position < number.size() &&
        (number[position] == 'e' || number[position] == 'E')) {
      ++position;
      if (position < number.size() &&
          (number[position] == '+' || number[position] == '-')) {
        ++position;
      }
      size_t exponent_digits = 0;
      while (position < number.size() && IsAsciiDigit(number[position])) {
        ++position;
        ++exponent_digits;
      }
      if (!exponent_digits) {
        return std::nullopt;
      }
    }
    if (position != number.size()) {
      return std::nullopt;
    }
    double value = 0;
    // Each character is ASCII, so this keeps the number as it is.
    base::StringToDouble(base::UTF16ToASCII(number), &value);
    if (!std::isfinite(value) ||
        std::abs(value) > std::numeric_limits<double>::max()) {
      return std::nullopt;
    }
    return static_cast<float>(value == 0 ? 0 : value);
  }
};

// What a candidate's descriptors say.
struct DescriptorParsingResult {
  std::optional<float> density;
  std::optional<int> resource_width;
  std::optional<int> resource_height;
};

enum class TokenizerState {
  kTokenStart,
  kInParenthesis,
  kAfterToken,
};

void AppendDescriptorAndReset(std::optional<size_t>& descriptor_start,
                              size_t position,
                              std::vector<DescriptorToken>& descriptors) {
  const size_t start = descriptor_start.value_or(0);
  if (position > start) {
    descriptors.push_back({start, position - start});
  }
  descriptor_start = std::nullopt;
}

void AppendCharacter(std::optional<size_t>& descriptor_start, size_t position) {
  if (!descriptor_start) {
    descriptor_start = position;
  }
}

void TokenizeDescriptors(std::u16string_view attribute,
                         size_t& position,
                         std::vector<DescriptorToken>& descriptors) {
  TokenizerState state = TokenizerState::kTokenStart;
  std::optional<size_t> current_start = position;
  const size_t size = attribute.size();
  while (true) {
    switch (state) {
      case TokenizerState::kTokenStart: {
        if (position >= size) {
          AppendDescriptorAndReset(current_start, size, descriptors);
          return;
        }
        const char16_t character = attribute[position];
        if (IsComma(character)) {
          AppendDescriptorAndReset(current_start, position, descriptors);
          ++position;
          return;
        }
        if (IsHTMLSpace(character)) {
          AppendDescriptorAndReset(current_start, position, descriptors);
          current_start = position + 1;
          state = TokenizerState::kAfterToken;
        } else if (character == '(') {
          AppendCharacter(current_start, position);
          state = TokenizerState::kInParenthesis;
        } else {
          AppendCharacter(current_start, position);
        }
        break;
      }
      case TokenizerState::kInParenthesis:
        if (position >= size) {
          AppendDescriptorAndReset(current_start, size, descriptors);
          return;
        }
        if (attribute[position] == ')') {
          AppendCharacter(current_start, position + 1);
          state = TokenizerState::kTokenStart;
        } else {
          AppendCharacter(current_start, position);
        }
        break;
      case TokenizerState::kAfterToken:
        if (position >= size) {
          return;
        }
        if (!IsHTMLSpace(attribute[position])) {
          state = TokenizerState::kTokenStart;
          current_start = position;
          --position;
        }
        break;
    }
    ++position;
  }
}

void SrcsetError(const SrcsetWarning& warn, std::string_view message) {
  if (warn) {
    warn.Run(base::StrCat(
        {"Failed parsing 'srcset' attribute value since ", message}));
  }
}

bool ParseDescriptors(std::u16string_view attribute,
                      const std::vector<DescriptorToken>& descriptors,
                      DescriptorParsingResult& result,
                      const SrcsetWarning& warn) {
  for (const DescriptorToken& descriptor : descriptors) {
    if (descriptor.length == 0) {
      continue;
    }
    const char16_t c = attribute[descriptor.LastIndex()];
    if (c == 'w') {
      if (result.density || result.resource_width) {
        SrcsetError(warn,
                    "it has multiple 'w' descriptors or a mix of 'x' and 'w' "
                    "descriptors.");
        return false;
      }
      const std::optional<int> width = descriptor.ToInt(attribute);
      if (!width || *width <= 0) {
        SrcsetError(warn, "its 'w' descriptor is invalid.");
        return false;
      }
      result.resource_width = width;
    } else if (c == 'h') {
      // Only for compatibility to come: its value is not used.
      if (result.density || result.resource_height) {
        SrcsetError(warn,
                    "it has multiple 'h' descriptors or a mix of 'x' and 'h' "
                    "descriptors.");
        return false;
      }
      const std::optional<int> height = descriptor.ToInt(attribute);
      if (!height || *height <= 0) {
        SrcsetError(warn, "its 'h' descriptor is invalid.");
        return false;
      }
      result.resource_height = height;
    } else if (c == 'x') {
      if (result.density || result.resource_height || result.resource_width) {
        SrcsetError(warn,
                    "it has multiple 'x' descriptors or a mix of 'x' and "
                    "'w'/'h' descriptors.");
        return false;
      }
      const std::optional<float> density = descriptor.ToFloat(attribute);
      if (!density || *density < 0) {
        SrcsetError(warn, "its 'x' descriptor is invalid.");
        return false;
      }
      result.density = density;
    } else {
      SrcsetError(warn, "it has an unknown descriptor.");
      return false;
    }
  }
  if (result.resource_height && !result.resource_width) {
    SrcsetError(warn, "it has an 'h' descriptor and no 'w' descriptor.");
    return false;
  }
  return true;
}

// JSONValue::QuoteString(): in quotation marks, with Blink's JSON escapes,
// and <, > and control characters as \u escapes.
std::string QuoteString(std::u16string_view text) {
  std::u16string quoted = u"\"";
  for (const char16_t c : text) {
    switch (c) {
      case '\b':
        quoted += u"\\b";
        break;
      case '\f':
        quoted += u"\\f";
        break;
      case '\n':
        quoted += u"\\n";
        break;
      case '\r':
        quoted += u"\\r";
        break;
      case '\t':
        quoted += u"\\t";
        break;
      case '\\':
        quoted += u"\\\\";
        break;
      case '"':
        quoted += u"\\\"";
        break;
      default:
        if (c < 32 || c == '<' || c == '>') {
          constexpr char16_t kHexDigits[] = u"0123456789ABCDEF";
          quoted += u"\\u";
          for (int shift = 12; shift >= 0; shift -= 4) {
            quoted += kHexDigits[(c >> shift) & 0xF];
          }
        } else {
          quoted += c;
        }
    }
  }
  quoted += '"';
  return base::UTF16ToUTF8(quoted);
}

// SrcsetSelectionMatchesImageSet, stable in Chrome 157: the first candidate
// whose density reaches the display's, or else the densest.
size_t SelectionLogic(const std::vector<ImageCandidate*>& candidates,
                      float device_scale_factor) {
  size_t i = 0;
  for (; i < candidates.size() - 1; ++i) {
    if (candidates[i]->Density() >= device_scale_factor) {
      return i;
    }
  }
  return i;
}

size_t AvoidDownloadIfHigherDensityResourceIsInCache(
    const std::vector<ImageCandidate*>& candidates,
    size_t winner,
    const SrcsetImageAvailable& available) {
  if (!available) {
    return winner;
  }
  for (size_t i = candidates.size() - 1; i > winner; --i) {
    if (available.Run(*candidates[i]->Url())) {
      return i;
    }
  }
  return winner;
}

ImageCandidate PickBestImageCandidate(float device_scale_factor,
                                      float source_size,
                                      std::vector<ImageCandidate>& candidates,
                                      const SrcsetImageAvailable& available) {
  constexpr float kDefaultDensityValue = 1.0;
  if (candidates.empty()) {
    return ImageCandidate();
  }
  bool ignore_src = false;
  // https://html.spec.whatwg.org/#normalise-the-source-densities
  for (ImageCandidate& image : candidates) {
    if (image.GetResourceWidth() > 0) {
      image.SetDensity(static_cast<float>(image.GetResourceWidth()) /
                       source_size);
      ignore_src = true;
    } else if (image.Density() < 0) {
      image.SetDensity(kDefaultDensityValue);
    }
  }
  std::ranges::stable_sort(candidates, {}, &ImageCandidate::Density);
  std::vector<ImageCandidate*> de_duped;
  float prev_density = -1.0;
  for (ImageCandidate& image : candidates) {
    if (image.Density() != prev_density &&
        (!ignore_src || !image.SrcOrigin())) {
      de_duped.push_back(&image);
    }
    prev_density = image.Density();
  }
  size_t winner = SelectionLogic(de_duped, device_scale_factor);
  winner = AvoidDownloadIfHigherDensityResourceIsInCache(de_duped, winner,
                                                         available);
  const float winning_density = de_duped[winner]->Density();
  // An entry of the same density as an earlier one goes.
  while (winner > 0 && de_duped[winner - 1]->Density() == winning_density) {
    --winner;
  }
  return *de_duped[winner];
}

}  // namespace

ImageCandidate::ImageCandidate() = default;

ImageCandidate::ImageCandidate(std::u16string url,
                               std::optional<float> density,
                               std::optional<int> resource_width,
                               Origin origin)
    : url_(std::move(url)),
      density_(density.value_or(-1)),
      resource_width_(resource_width.value_or(0)),
      origin_(origin) {}

ImageCandidate::ImageCandidate(const ImageCandidate&) = default;
ImageCandidate& ImageCandidate::operator=(const ImageCandidate&) = default;
ImageCandidate::~ImageCandidate() = default;

// https://html.spec.whatwg.org/#parse-a-srcset-attribute
std::vector<ImageCandidate> ParseImageCandidatesFromSrcsetAttribute(
    std::u16string_view attribute,
    const SrcsetWarning& warn) {
  std::vector<ImageCandidate> candidates;
  size_t position = 0;
  const size_t size = attribute.size();
  while (position < size) {
    // Splitting loop: white space and commas.
    while (position < size && IsHTMLSpaceOrComma(attribute[position])) {
      ++position;
    }
    if (position == size) {
      break;
    }
    const size_t url_start = position;
    // The URL: what is not white space.
    while (position < size && !IsHTMLSpace(attribute[position])) {
      ++position;
    }
    size_t url_end = position;
    DescriptorParsingResult result;
    if (IsComma(attribute[position - 1])) {
      // Trailing commas go, and a URL of nothing goes with them.
      url_end = position - 1;
      while (url_end > url_start && IsComma(attribute[url_end - 1])) {
        --url_end;
      }
      if (url_start == url_end) {
        continue;
      }
    } else {
      while (position < size && IsHTMLSpace(attribute[position])) {
        ++position;
      }
      std::vector<DescriptorToken> descriptors;
      TokenizeDescriptors(attribute, position, descriptors);
      if (!ParseDescriptors(attribute, descriptors, result, warn)) {
        if (warn) {
          warn.Run(base::StrCat(
              {"Dropped srcset candidate ",
               QuoteString(attribute.substr(url_start, url_end - url_start))}));
        }
        continue;
      }
    }
    candidates.emplace_back(
        std::u16string(attribute.substr(url_start, url_end - url_start)),
        result.density, result.resource_width, ImageCandidate::Origin::kSrcset);
  }
  return candidates;
}

ImageCandidate BestFitSourceForSrcsetAttribute(
    float device_scale_factor,
    float source_size,
    std::u16string_view srcset_attribute,
    const SrcsetWarning& warn,
    const SrcsetImageAvailable& available) {
  std::vector<ImageCandidate> candidates =
      ParseImageCandidatesFromSrcsetAttribute(srcset_attribute, warn);
  return PickBestImageCandidate(device_scale_factor, source_size, candidates,
                                available);
}

ImageCandidate BestFitSourceForImageAttributes(
    float device_scale_factor,
    float source_size,
    const std::optional<std::u16string>& src_attribute,
    const std::optional<std::u16string>& srcset_attribute,
    const SrcsetWarning& warn,
    const SrcsetImageAvailable& available) {
  if (!srcset_attribute) {
    if (!src_attribute) {
      return ImageCandidate();
    }
    return ImageCandidate(*src_attribute, std::nullopt, std::nullopt,
                          ImageCandidate::Origin::kSrc);
  }
  std::vector<ImageCandidate> candidates =
      ParseImageCandidatesFromSrcsetAttribute(*srcset_attribute, warn);
  if (src_attribute && !src_attribute->empty()) {
    candidates.emplace_back(*src_attribute, std::nullopt, std::nullopt,
                            ImageCandidate::Origin::kSrc);
  }
  return PickBestImageCandidate(device_scale_factor, source_size, candidates,
                                available);
}

}  // namespace mitto
