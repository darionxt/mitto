// Media Queries' parser, as Blink's MediaQueryParser parses a media query
// list for matchMedia() and a media attribute:
// https://drafts.csswg.org/mediaqueries-5/#mq-syntax
//
// A query that does not parse becomes "not all", and a condition of a
// syntax that Blink does not know becomes <general-enclosed>, which is
// unknown when evaluated and written back as it was given. Which media
// features parse follows the runtime-enabled features the parser is given.
//
// Upstream: third_party/blink/renderer/core/css/parser/media_query_parser.cc
// Upstream: third_party/blink/renderer/core/css/parser/conditional_parser.cc
// Upstream: third_party/blink/renderer/core/css/media_query_exp.cc
// Upstream: third_party/blink/renderer/core/css/properties/css_parsing_utils.cc

#ifndef MITTO_CSS_MEDIA_QUERY_PARSER_H_
#define MITTO_CSS_MEDIA_QUERY_PARSER_H_

#include <memory>
#include <string_view>

#include "mitto/css/media_query.h"

namespace mitto {

// The runtime-enabled features that decide which media features parse, as
// Chrome's stable set on Windows has them unless a Page's set says else.
struct MediaQueryFeatures {
  bool forced_colors = true;
  bool inverted_colors = false;
  bool prefers_reduced_data = false;
  bool video_dynamic_range = false;
  bool navigation_controls = false;
  bool viewport_segments = true;
  bool device_posture = true;
  bool additional_windowing_controls = false;
  bool scrolled_container_queries = true;
};

// Parses a media query list. An empty or blank one gives an empty list,
// which matches.
std::unique_ptr<MediaQuerySet> ParseMediaQuerySet(
    std::u16string_view input,
    const MediaQueryFeatures& features);

}  // namespace mitto

#endif  // MITTO_CSS_MEDIA_QUERY_PARSER_H_
