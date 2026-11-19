// What Blink's image decoders read of an image before they decode a pixel:
// which decoder takes it, by its first bytes, and its size, with the
// orientation and density that its EXIF metadata gives. An SVG image's
// natural dimensions come from its root element, as Blink's SVGImage lays
// it out. The Page decodes no pixels, so an image whose header reads is
// one that loads.
//
// Upstream: third_party/blink/renderer/platform/image-decoders/image_decoder.cc
// Upstream: third_party/blink/renderer/platform/graphics/bitmap_image.cc
// Upstream: third_party/blink/renderer/core/svg/graphics/svg_image.cc
// Upstream: third_party/blink/renderer/core/layout/svg/layout_svg_root.cc
// Upstream: third_party/blink/renderer/core/layout/natural_sizing_info.cc

#ifndef MITTO_DOM_IMAGE_HEADER_H_
#define MITTO_DOM_IMAGE_HEADER_H_

#include <optional>
#include <string_view>

namespace mitto {

// Blink's NaturalSizingInfo: natural dimensions that an SVG image may lack,
// and a natural aspect ratio.
struct NaturalSizingInfo {
  float width = 0;
  float height = 0;
  bool has_width = false;
  bool has_height = false;
  // Width over height, as a width and a height; both 0 for none.
  float aspect_width = 0;
  float aspect_height = 0;
};

struct ImageHeader {
  enum class Kind {
    kBitmap,
    kSvg,
  };

  Kind kind = Kind::kBitmap;
  // A bitmap's size as Blink's Image::Size(kRespectImageOrientation) gives
  // it: density corrected by its EXIF resolution, then turned by its
  // orientation.
  int width = 0;
  int height = 0;
  // An SVG image's natural dimensions.
  NaturalSizingInfo sizing;
};

// The header of `body`, whole as the response gave it, with the response's
// MIME type essence `mime_type`: an SVG image for image/svg+xml, and a
// bitmap that a decoder of Chrome's takes otherwise. Nullopt for an image
// that Blink would fail with a decode error: no decoder for its bytes, no
// size, or an SVG image without an svg root element.
std::optional<ImageHeader> ReadImageHeader(std::string_view body,
                                           std::string_view mime_type);

// Blink's ConcreteObjectSize(): the CSS default sizing algorithm, for an
// object with `sizing` in a box of `default_width` by `default_height`.
void ConcreteObjectSize(const NaturalSizingInfo& sizing,
                        float default_width,
                        float default_height,
                        float& width,
                        float& height);

}  // namespace mitto

#endif  // MITTO_DOM_IMAGE_HEADER_H_
