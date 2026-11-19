// The sizes attribute of an img or source element, as Blink's
// SizesAttributeParser parses it: a list of a media condition and a length,
// whose first match gives the source size that srcset's width descriptors
// divide, 100vw for none, and auto, which an img that allows auto-sizes
// takes from its layout box.
//
// Upstream: third_party/blink/renderer/core/css/parser/sizes_attribute_parser.cc
// Upstream: third_party/blink/renderer/core/css/parser/sizes_math_function_parser.cc

#ifndef MITTO_CSS_SIZES_ATTRIBUTE_PARSER_H_
#define MITTO_CSS_SIZES_ATTRIBUTE_PARSER_H_

#include <optional>
#include <string_view>

#include "base/memory/raw_ref.h"
#include "mitto/css/media_query_parser.h"
#include "mitto/css/media_values.h"

namespace mitto {

class CssTokenStream;

// What an img element tells the parser of itself, for auto.
struct SizesAutoContext {
  // Whether the img allows auto-sizes: lazy, and its sizes is auto or
  // starts with "auto,".
  bool allows_auto_sizes = false;
  // Its layout box's width while it is being rendered, which a Page without
  // layout never has.
  std::optional<float> rendered_width;
  // Its last auto-sizes width.
  std::optional<float> last_auto_sizes_width;
};

class SizesAttributeParser {
 public:
  // `attribute` may be null, for an element without one, which gives 100vw.
  SizesAttributeParser(const MediaValues& media_values,
                       const MediaQueryFeatures& features,
                       std::optional<std::u16string_view> attribute,
                       const SizesAutoContext& auto_context = {});
  SizesAttributeParser(const SizesAttributeParser&) = delete;
  SizesAttributeParser& operator=(const SizesAttributeParser&) = delete;
  ~SizesAttributeParser();

  // The source size in CSS pixels.
  float Size() const;
  bool IsAuto() const { return is_auto_; }

 private:
  bool Parse(CssTokenStream& stream);
  bool CalculateLengthInPixels(CssTokenStream& stream, float& result) const;
  float EffectiveSize() const;
  float EffectiveSizeDefaultValue() const;

  const raw_ref<const MediaValues> media_values_;
  const raw_ref<const MediaQueryFeatures> features_;
  const SizesAutoContext auto_context_;
  float size_ = 0;
  bool size_was_set_ = false;
  bool is_valid_ = false;
  bool is_auto_ = false;
};

}  // namespace mitto

#endif  // MITTO_CSS_SIZES_ATTRIBUTE_PARSER_H_
