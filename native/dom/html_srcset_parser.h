// The srcset attribute of img and source elements, as Blink's
// HTMLSrcsetParser parses it and picks the image candidate for the
// display's density.
//
// Upstream: third_party/blink/renderer/core/html/parser/html_srcset_parser.h
// Upstream: third_party/blink/renderer/core/html/parser/html_srcset_parser.cc

#ifndef MITTO_DOM_HTML_SRCSET_PARSER_H_
#define MITTO_DOM_HTML_SRCSET_PARSER_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"

namespace mitto {

// An image candidate: a URL of srcset or src, and its descriptors.
class ImageCandidate {
 public:
  enum class Origin {
    kSrc,
    kSrcset,
  };

  ImageCandidate();
  ImageCandidate(std::u16string url,
                 std::optional<float> density,
                 std::optional<int> resource_width,
                 Origin origin);
  ImageCandidate(const ImageCandidate&);
  ImageCandidate& operator=(const ImageCandidate&);
  ~ImageCandidate();

  // The URL as written, without white space around it. Null for none.
  const std::optional<std::u16string>& Url() const { return url_; }
  bool IsEmpty() const { return !url_ || url_->empty(); }
  // -1 when no density is known.
  float Density() const { return density_; }
  void SetDensity(float density) { density_ = density; }
  // 0 without a width descriptor.
  int GetResourceWidth() const { return resource_width_; }
  bool SrcOrigin() const { return origin_ == Origin::kSrc; }

 private:
  std::optional<std::u16string> url_;
  float density_ = -1;
  int resource_width_ = 0;
  Origin origin_ = Origin::kSrc;
};

// Where the parser says why it dropped a candidate, as Blink's console
// warnings say it. Null says nothing.
using SrcsetWarning = base::RepeatingCallback<void(std::string)>;

// The candidates of a srcset attribute, in order, as the HTML Standard's
// "parse a srcset attribute" makes them.
std::vector<ImageCandidate> ParseImageCandidatesFromSrcsetAttribute(
    std::u16string_view attribute,
    const SrcsetWarning& warn = {});

// What tells the picker that a candidate's image is ready to use without a
// download: in the list of available images, or a data: URL. Null for none.
using SrcsetImageAvailable =
    base::RepeatingCallback<bool(const std::u16string& url)>;

// The best candidate of a source element's srcset for `device_scale_factor`,
// with width descriptors divided by `source_size`.
ImageCandidate BestFitSourceForSrcsetAttribute(
    float device_scale_factor,
    float source_size,
    std::u16string_view srcset_attribute,
    const SrcsetWarning& warn = {},
    const SrcsetImageAvailable& available = {});

// The best candidate of an img's srcset and src, either of which may be null
// for no attribute.
ImageCandidate BestFitSourceForImageAttributes(
    float device_scale_factor,
    float source_size,
    const std::optional<std::u16string>& src_attribute,
    const std::optional<std::u16string>& srcset_attribute,
    const SrcsetWarning& warn = {},
    const SrcsetImageAvailable& available = {});

}  // namespace mitto

#endif  // MITTO_DOM_HTML_SRCSET_PARSER_H_
