#include "mitto/css/media_values.h"

namespace mitto {

MediaValues::MediaValues() = default;
MediaValues::MediaValues(const MediaValues&) = default;
MediaValues& MediaValues::operator=(const MediaValues&) = default;
MediaValues::~MediaValues() = default;

// A media query's lengths resolve against the root element's font, so the
// root forms of the units equal the others.
// Upstream: third_party/blink/renderer/core/css/media_values.cc
CssLengthResolver MediaValues::LengthResolver() const {
  CssLengthResolver resolver;
  resolver.em = em_size;
  resolver.rem = em_size;
  resolver.ex = ex_size;
  resolver.rex = ex_size;
  resolver.ch = ch_size;
  resolver.rch = ch_size;
  resolver.ic = ic_size;
  resolver.ric = ic_size;
  resolver.cap = cap_size;
  resolver.rcap = cap_size;
  resolver.lh = line_height;
  resolver.rlh = line_height;
  resolver.viewport_width = viewport_width;
  resolver.viewport_height = viewport_height;
  return resolver;
}

}  // namespace mitto
