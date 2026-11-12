#include "mitto/css/media_query_evaluator.h"

#include <cmath>
#include <limits>
#include <string_view>

#include "base/numerics/safe_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"

namespace mitto {

namespace {

using Op = MediaQueryOperator;
using EvalFunc = bool (*)(const MediaQueryExpValue&,
                          MediaQueryOperator,
                          const MediaValues&);

// LayoutUnit::Epsilon(): a 64th of a pixel.
constexpr double kLayoutUnitEpsilon = 1.0 / 64;
// Upstream: third_party/blink/renderer/core/css/css_resolution_units.h
constexpr float kCssPixelsPerInch = 96;

// ClampTo<float>(): held to the floats, with NaN left as it is.
float ClampToFloat(double value) {
  if (value >= std::numeric_limits<float>::max()) {
    return std::numeric_limits<float>::max();
  }
  if (value <= std::numeric_limits<float>::lowest()) {
    return std::numeric_limits<float>::lowest();
  }
  return static_cast<float>(value);
}

// ClampTo<int>(): saturated, and 0 for NaN.
int ClampToInt(double value) {
  return base::saturated_cast<int>(value);
}

bool IsMediaType(const MediaValues& media_values, std::string_view type) {
  return base::EqualsCaseInsensitiveASCII(media_values.media_type, type);
}

// https://w3c.github.io/csswg-drafts/mediaqueries/#false-in-the-negative-range
bool HandleNegativeMediaFeatureValue(Op op) {
  switch (op) {
    case Op::kLe:
    case Op::kLt:
    case Op::kEq:
    case Op::kNone:
      return false;
    case Op::kGt:
    case Op::kGe:
      return true;
  }
  return false;
}

template <typename T>
bool CompareValue(T actual_value, T query_value, Op op) {
  if (query_value < T(0)) {
    return HandleNegativeMediaFeatureValue(op);
  }
  switch (op) {
    case Op::kGe:
      return actual_value >= query_value;
    case Op::kLe:
      return actual_value <= query_value;
    case Op::kEq:
    case Op::kNone:
      return actual_value == query_value;
    case Op::kLt:
      return actual_value < query_value;
    case Op::kGt:
      return actual_value > query_value;
  }
  return false;
}

bool CompareDoubleValue(double actual_value, double query_value, Op op) {
  if (query_value < 0) {
    return HandleNegativeMediaFeatureValue(op);
  }
  switch (op) {
    case Op::kGe:
      return actual_value >= (query_value - kLayoutUnitEpsilon);
    case Op::kLe:
      return actual_value <= (query_value + kLayoutUnitEpsilon);
    case Op::kEq:
    case Op::kNone:
      return std::abs(actual_value - query_value) <= kLayoutUnitEpsilon;
    case Op::kLt:
      return actual_value < query_value;
    case Op::kGt:
      return actual_value > query_value;
  }
  return false;
}

bool CompareAspectRatioValue(const MediaQueryExpValue& value,
                             int width,
                             int height,
                             Op op,
                             const MediaValues& media_values) {
  if (!value.IsRatio()) {
    return false;
  }
  const CssLengthResolver resolver = media_values.LengthResolver();
  return CompareDoubleValue(
      static_cast<double>(width) * value.Denominator(resolver),
      static_cast<double>(height) * value.Numerator(resolver), op);
}

bool NumberValue(const MediaQueryExpValue& value,
                 float& result,
                 const MediaValues& media_values) {
  if (value.IsNumber()) {
    result = ClampToFloat(value.Value(media_values.LengthResolver()));
    return true;
  }
  return false;
}

bool IdIs(const MediaQueryExpValue& value, std::u16string_view id) {
  return value.IsId() && value.Id() == id;
}

bool ColorMediaFeatureEval(const MediaQueryExpValue& value,
                           Op op,
                           const MediaValues& media_values) {
  float number;
  const int bits_per_component = media_values.color_bits_per_component;
  if (value.IsValid()) {
    return NumberValue(value, number, media_values) &&
           CompareValue(bits_per_component, ClampToInt(number), op);
  }
  return bits_per_component != 0;
}

bool ColorIndexMediaFeatureEval(const MediaQueryExpValue& value,
                                Op op,
                                const MediaValues& media_values) {
  // Blink assumes no indexed displays, as Firefox does.
  if (!value.IsValid()) {
    return false;
  }
  float number;
  return NumberValue(value, number, media_values) &&
         CompareValue(0, ClampToInt(number), op);
}

bool MonochromeMediaFeatureEval(const MediaQueryExpValue& value,
                                Op op,
                                const MediaValues& media_values) {
  float number;
  const int bits_per_component = media_values.monochrome_bits_per_component;
  if (value.IsValid()) {
    return NumberValue(value, number, media_values) &&
           CompareValue(bits_per_component, ClampToInt(number), op);
  }
  return bits_per_component != 0;
}

bool DisplayModeMediaFeatureEval(const MediaQueryExpValue& value,
                                 Op,
                                 const MediaValues& media_values) {
  if (!value.IsValid()) {
    return true;
  }
  if (!value.IsId()) {
    return false;
  }
  switch (media_values.display_mode) {
    case MediaDisplayMode::kFullscreen:
      return IdIs(value, u"fullscreen");
    case MediaDisplayMode::kStandalone:
      return IdIs(value, u"standalone");
    case MediaDisplayMode::kMinimalUi:
      return IdIs(value, u"minimal-ui");
    case MediaDisplayMode::kBrowser:
      return IdIs(value, u"browser");
    case MediaDisplayMode::kWindowControlsOverlay:
      return IdIs(value, u"window-controls-overlay");
    case MediaDisplayMode::kTabbed:
      return IdIs(value, u"tabbed");
    case MediaDisplayMode::kPictureInPicture:
      return IdIs(value, u"picture-in-picture");
    case MediaDisplayMode::kUnframed:
      return IdIs(value, u"unframed");
  }
  return false;
}

bool DisplayStateMediaFeatureEval(const MediaQueryExpValue& value,
                                  Op,
                                  const MediaValues& media_values) {
  if (!value.IsValid()) {
    return true;
  }
  if (!value.IsId()) {
    return false;
  }
  const MediaWindowShowState state = media_values.window_show_state;
  if (IdIs(value, u"fullscreen")) {
    return state == MediaWindowShowState::kFullscreen;
  }
  if (IdIs(value, u"maximized")) {
    return state == MediaWindowShowState::kMaximized;
  }
  if (IdIs(value, u"minimized")) {
    return state == MediaWindowShowState::kMinimized;
  }
  return state == MediaWindowShowState::kDefault ||
         state == MediaWindowShowState::kInactive ||
         state == MediaWindowShowState::kNormal;
}

bool ResizableMediaFeatureEval(const MediaQueryExpValue& value,
                               Op,
                               const MediaValues& media_values) {
  const bool resizable = media_values.resizable;
  if (!value.IsValid()) {
    return resizable;
  }
  if (!value.IsId()) {
    return false;
  }
  return (resizable && IdIs(value, u"true")) ||
         (!resizable && IdIs(value, u"false"));
}

bool OrientationMediaFeatureEval(const MediaQueryExpValue& value,
                                 Op,
                                 const MediaValues& media_values) {
  const double width = media_values.viewport_width;
  const double height = media_values.viewport_height;
  if (value.IsId()) {
    // A square viewport is portrait.
    if (width > height) {
      return IdIs(value, u"landscape");
    }
    return IdIs(value, u"portrait");
  }
  return height >= 0 && width >= 0;
}

bool AspectRatioMediaFeatureEval(const MediaQueryExpValue& value,
                                 Op op,
                                 const MediaValues& media_values) {
  if (value.IsValid()) {
    return CompareAspectRatioValue(
        value, static_cast<int>(media_values.viewport_width),
        static_cast<int>(media_values.viewport_height), op, media_values);
  }
  return true;
}

bool DeviceAspectRatioMediaFeatureEval(const MediaQueryExpValue& value,
                                       Op op,
                                       const MediaValues& media_values) {
  if (value.IsValid()) {
    return CompareAspectRatioValue(value, media_values.device_width,
                                   media_values.device_height, op,
                                   media_values);
  }
  return true;
}

bool DynamicRangeMediaFeatureEval(const MediaQueryExpValue& value,
                                  Op,
                                  const MediaValues& media_values) {
  if (!value.IsValid()) {
    return true;
  }
  if (IdIs(value, u"standard")) {
    return true;
  }
  return IdIs(value, u"high") && media_values.device_supports_hdr;
}

bool EvalResolution(const MediaQueryExpValue& value,
                    Op op,
                    const MediaValues& media_values) {
  // Only screen and print match.
  float actual_resolution = 0;
  if (IsMediaType(media_values, "screen")) {
    actual_resolution = ClampToFloat(media_values.device_pixel_ratio);
  } else if (IsMediaType(media_values, "print")) {
    // Printing takes 300 dpi, as Blink does.
    actual_resolution = 300 / kCssPixelsPerInch;
  }
  if (!value.IsValid()) {
    return actual_resolution != 0;
  }
  const CssLengthResolver resolver = media_values.LengthResolver();
  if (value.IsNumber()) {
    return CompareValue(actual_resolution, ClampToFloat(value.Value(resolver)),
                        op);
  }
  if (!value.IsResolution()) {
    return false;
  }
  // Resolutions are in dppx, their canonical unit.
  const float value_in_dppx = ClampToFloat(value.Value(resolver));
  if (value.IsDotsPerCentimeter()) {
    // dpcm compares to two decimal places.
    return CompareValue(
        std::floor(static_cast<float>(0.5 + 100 * actual_resolution)) / 100,
        std::floor(static_cast<float>(0.5 + 100 * value_in_dppx)) / 100, op);
  }
  return CompareValue(actual_resolution, value_in_dppx, op);
}

bool DevicePixelRatioMediaFeatureEval(const MediaQueryExpValue& value,
                                      Op op,
                                      const MediaValues& media_values) {
  return (!value.IsValid() || value.IsNumber()) &&
         EvalResolution(value, op, media_values);
}

bool ResolutionMediaFeatureEval(const MediaQueryExpValue& value,
                                Op op,
                                const MediaValues& media_values) {
  return (!value.IsValid() || value.IsResolution()) &&
         EvalResolution(value, op, media_values);
}

bool GridMediaFeatureEval(const MediaQueryExpValue& value,
                          Op op,
                          const MediaValues& media_values) {
  // A bitmap device: (grid: 0) matches.
  float number;
  if (value.IsValid() && NumberValue(value, number, media_values)) {
    return CompareValue(ClampToInt(number), 0, op);
  }
  return false;
}

bool ComputeLength(const MediaQueryExpValue& value,
                   const MediaValues& media_values,
                   double& result) {
  const CssLengthResolver resolver = media_values.LengthResolver();
  if (value.IsNumber()) {
    result = ClampToInt(value.Value(resolver));
    return !media_values.strict_mode || !result;
  }
  if (value.IsValue()) {
    result = value.Value(resolver);
    return true;
  }
  return false;
}

bool ComputeLengthAndCompare(const MediaQueryExpValue& value,
                             Op op,
                             const MediaValues& media_values,
                             double compare_to_value) {
  double length;
  return ComputeLength(value, media_values, length) &&
         CompareDoubleValue(compare_to_value, length, op);
}

bool DeviceHeightMediaFeatureEval(const MediaQueryExpValue& value,
                                  Op op,
                                  const MediaValues& media_values) {
  if (value.IsValid()) {
    return ComputeLengthAndCompare(value, op, media_values,
                                   media_values.device_height);
  }
  return true;
}

bool DeviceWidthMediaFeatureEval(const MediaQueryExpValue& value,
                                 Op op,
                                 const MediaValues& media_values) {
  if (value.IsValid()) {
    return ComputeLengthAndCompare(value, op, media_values,
                                   media_values.device_width);
  }
  return true;
}

bool HeightMediaFeatureEval(const MediaQueryExpValue& value,
                            Op op,
                            const MediaValues& media_values) {
  const double height = media_values.viewport_height;
  if (value.IsValid()) {
    return ComputeLengthAndCompare(value, op, media_values, height);
  }
  return height != 0;
}

bool WidthMediaFeatureEval(const MediaQueryExpValue& value,
                           Op op,
                           const MediaValues& media_values) {
  const double width = media_values.viewport_width;
  if (value.IsValid()) {
    return ComputeLengthAndCompare(value, op, media_values, width);
  }
  return width != 0;
}

// The min- and max- prefixes compare as >= and <=.
template <EvalFunc Func, Op PrefixOp>
bool Prefixed(const MediaQueryExpValue& value,
              Op,
              const MediaValues& media_values) {
  return Func(value, PrefixOp, media_values);
}

bool Transform3dMediaFeatureEval(const MediaQueryExpValue& value,
                                 Op op,
                                 const MediaValues& media_values) {
  const bool three_d_enabled = media_values.three_d_enabled;
  if (value.IsValid()) {
    float number;
    return NumberValue(value, number, media_values) &&
           CompareValue(three_d_enabled ? 1 : 0, ClampToInt(number), op);
  }
  return three_d_enabled;
}

bool HoverMediaFeatureEval(const MediaQueryExpValue& value,
                           Op,
                           const MediaValues& media_values) {
  const int hover = media_values.primary_hover_type;
  if (!value.IsValid()) {
    return hover != kHoverTypeNone;
  }
  if (!value.IsId()) {
    return false;
  }
  return (hover == kHoverTypeNone && IdIs(value, u"none")) ||
         (hover == kHoverTypeHover && IdIs(value, u"hover"));
}

bool AnyHoverMediaFeatureEval(const MediaQueryExpValue& value,
                              Op,
                              const MediaValues& media_values) {
  const int available_hover_types = media_values.available_hover_types;
  if (!value.IsValid()) {
    return available_hover_types & ~kHoverTypeNone;
  }
  if (!value.IsId()) {
    return false;
  }
  if (IdIs(value, u"none")) {
    return available_hover_types & kHoverTypeNone;
  }
  return available_hover_types & kHoverTypeHover;
}

bool PointerMediaFeatureEval(const MediaQueryExpValue& value,
                             Op,
                             const MediaValues& media_values) {
  const int pointer = media_values.primary_pointer_type;
  if (!value.IsValid()) {
    return pointer != kPointerTypeNone;
  }
  if (!value.IsId()) {
    return false;
  }
  return (pointer == kPointerTypeNone && IdIs(value, u"none")) ||
         (pointer == kPointerTypeCoarse && IdIs(value, u"coarse")) ||
         (pointer == kPointerTypeFine && IdIs(value, u"fine"));
}

bool AnyPointerMediaFeatureEval(const MediaQueryExpValue& value,
                                Op,
                                const MediaValues& media_values) {
  const int available_pointers = media_values.available_pointer_types;
  if (!value.IsValid()) {
    return available_pointers & ~kPointerTypeNone;
  }
  if (!value.IsId()) {
    return false;
  }
  if (IdIs(value, u"coarse")) {
    return available_pointers & kPointerTypeCoarse;
  }
  if (IdIs(value, u"fine")) {
    return available_pointers & kPointerTypeFine;
  }
  return available_pointers & kPointerTypeNone;
}

// "no-preference" against a setting that is on or off.
bool PreferenceEval(const MediaQueryExpValue& value, bool setting) {
  if (!value.IsValid()) {
    return setting;
  }
  if (!value.IsId()) {
    return false;
  }
  return IdIs(value, u"no-preference") ^ setting;
}

bool PrefersReducedMotionMediaFeatureEval(const MediaQueryExpValue& value,
                                          Op,
                                          const MediaValues& media_values) {
  return PreferenceEval(value, media_values.prefers_reduced_motion);
}

bool PrefersReducedDataMediaFeatureEval(const MediaQueryExpValue& value,
                                        Op,
                                        const MediaValues& media_values) {
  return PreferenceEval(value, media_values.prefers_reduced_data);
}

bool PrefersReducedTransparencyMediaFeatureEval(
    const MediaQueryExpValue& value,
    Op,
    const MediaValues& media_values) {
  return PreferenceEval(value, media_values.prefers_reduced_transparency);
}

bool ScanMediaFeatureEval(const MediaQueryExpValue& value,
                          Op,
                          const MediaValues& media_values) {
  // Scan applies only to tv, which is taken to be progressive.
  if (!IsMediaType(media_values, "tv")) {
    return false;
  }
  if (!value.IsValid()) {
    return true;
  }
  return IdIs(value, u"progressive");
}

bool ColorGamutMediaFeatureEval(const MediaQueryExpValue& value,
                                Op,
                                const MediaValues& media_values) {
  if (!value.IsValid()) {
    return true;
  }
  if (!value.IsId()) {
    return false;
  }
  switch (media_values.color_gamut) {
    case MediaColorGamut::kSrgb:
      return IdIs(value, u"srgb");
    case MediaColorGamut::kP3:
      return IdIs(value, u"srgb") || IdIs(value, u"p3");
    case MediaColorGamut::kBt2020:
      return IdIs(value, u"srgb") || IdIs(value, u"p3") ||
             IdIs(value, u"rec2020");
  }
  return false;
}

bool PrefersColorSchemeMediaFeatureEval(const MediaQueryExpValue& value,
                                        Op,
                                        const MediaValues& media_values) {
  if (!value.IsValid()) {
    return true;
  }
  if (!value.IsId()) {
    return false;
  }
  return (media_values.preferred_color_scheme == MediaColorScheme::kDark &&
          IdIs(value, u"dark")) ||
         (media_values.preferred_color_scheme == MediaColorScheme::kLight &&
          IdIs(value, u"light"));
}

bool PrefersContrastMediaFeatureEval(const MediaQueryExpValue& value,
                                     Op,
                                     const MediaValues& media_values) {
  const MediaContrast contrast = media_values.preferred_contrast;
  if (!value.IsValid()) {
    return contrast != MediaContrast::kNoPreference;
  }
  if (!value.IsId()) {
    return false;
  }
  if (IdIs(value, u"more")) {
    return contrast == MediaContrast::kMore;
  }
  if (IdIs(value, u"less")) {
    return contrast == MediaContrast::kLess;
  }
  if (IdIs(value, u"no-preference")) {
    return contrast == MediaContrast::kNoPreference;
  }
  return contrast == MediaContrast::kCustom;
}

bool ForcedColorsMediaFeatureEval(const MediaQueryExpValue& value,
                                  Op,
                                  const MediaValues& media_values) {
  const bool forced_colors = media_values.forced_colors;
  if (!value.IsValid()) {
    return forced_colors;
  }
  if (!value.IsId()) {
    return false;
  }
  return (!forced_colors && IdIs(value, u"none")) ||
         (forced_colors && IdIs(value, u"active"));
}

bool NavigationControlsMediaFeatureEval(const MediaQueryExpValue& value,
                                        Op,
                                        const MediaValues& media_values) {
  const bool back_button = media_values.navigation_controls_back_button;
  if (!value.IsValid()) {
    return back_button;
  }
  if (!value.IsId()) {
    return false;
  }
  return (!back_button && IdIs(value, u"none")) ||
         (back_button && IdIs(value, u"back-button"));
}

bool ViewportSegmentsEval(const MediaQueryExpValue& value,
                          Op op,
                          int segments,
                          const MediaValues& media_values) {
  if (!value.IsValid()) {
    return true;
  }
  float number;
  return NumberValue(value, number, media_values) &&
         CompareValue(segments, ClampToInt(number), op);
}

bool HorizontalViewportSegmentsMediaFeatureEval(
    const MediaQueryExpValue& value,
    Op op,
    const MediaValues& media_values) {
  return ViewportSegmentsEval(
      value, op, media_values.horizontal_viewport_segments, media_values);
}

bool VerticalViewportSegmentsMediaFeatureEval(const MediaQueryExpValue& value,
                                              Op op,
                                              const MediaValues& media_values) {
  return ViewportSegmentsEval(
      value, op, media_values.vertical_viewport_segments, media_values);
}

bool OverflowInlineMediaFeatureEval(const MediaQueryExpValue& value,
                                    Op,
                                    const MediaValues& media_values) {
  const bool can_scroll = !IsMediaType(media_values, "print");
  if (!value.IsValid()) {
    return can_scroll;
  }
  if (IdIs(value, u"none")) {
    return !can_scroll;
  }
  return can_scroll;
}

bool OverflowBlockMediaFeatureEval(const MediaQueryExpValue& value,
                                   Op,
                                   const MediaValues& media_values) {
  const bool can_scroll = !IsMediaType(media_values, "print");
  if (!value.IsValid()) {
    return true;
  }
  if (IdIs(value, u"none")) {
    return false;
  }
  if (IdIs(value, u"scroll")) {
    return can_scroll;
  }
  return !can_scroll;
}

bool DevicePostureMediaFeatureEval(const MediaQueryExpValue& value,
                                   Op,
                                   const MediaValues& media_values) {
  if (!value.IsValid()) {
    return true;
  }
  if (IdIs(value, u"continuous")) {
    return media_values.device_posture == MediaDevicePosture::kContinuous;
  }
  return media_values.device_posture == MediaDevicePosture::kFolded;
}

bool UpdateMediaFeatureEval(const MediaQueryExpValue& value,
                            Op,
                            const MediaValues& media_values) {
  const bool can_update = !IsMediaType(media_values, "print");
  if (!value.IsValid()) {
    return can_update;
  }
  if (IdIs(value, u"none")) {
    return !can_update;
  }
  if (IdIs(value, u"slow")) {
    return can_update && media_values.update == MediaUpdate::kSlow;
  }
  return can_update && media_values.update == MediaUpdate::kFast;
}

bool InvertedColorsMediaFeatureEval(const MediaQueryExpValue& value,
                                    Op,
                                    const MediaValues& media_values) {
  if (!value.IsValid()) {
    return media_values.inverted_colors;
  }
  if (!value.IsId()) {
    return false;
  }
  return IdIs(value, u"none") != media_values.inverted_colors;
}

bool ScriptingMediaFeatureEval(const MediaQueryExpValue& value,
                               Op,
                               const MediaValues& media_values) {
  if (!value.IsValid()) {
    return media_values.scripting == MediaScripting::kEnabled;
  }
  if (!value.IsId()) {
    return false;
  }
  if (IdIs(value, u"none")) {
    return media_values.scripting == MediaScripting::kNone;
  }
  if (IdIs(value, u"initial-only")) {
    return media_values.scripting == MediaScripting::kInitialOnly;
  }
  return media_values.scripting == MediaScripting::kEnabled;
}

struct FeatureFunction {
  std::u16string_view name;
  EvalFunc func;
};

// As Blink's CSS_MEDIAQUERY_NAMES_FOR_EACH_MEDIAFEATURE.
constexpr FeatureFunction kFeatureFunctions[] = {
    {u"color", ColorMediaFeatureEval},
    {u"color-gamut", ColorGamutMediaFeatureEval},
    {u"grid", GridMediaFeatureEval},
    {u"monochrome", MonochromeMediaFeatureEval},
    {u"height", HeightMediaFeatureEval},
    {u"hover", HoverMediaFeatureEval},
    {u"width", WidthMediaFeatureEval},
    {u"orientation", OrientationMediaFeatureEval},
    {u"aspect-ratio", AspectRatioMediaFeatureEval},
    {u"device-aspect-ratio", DeviceAspectRatioMediaFeatureEval},
    {u"-webkit-device-pixel-ratio", DevicePixelRatioMediaFeatureEval},
    {u"device-height", DeviceHeightMediaFeatureEval},
    {u"device-width", DeviceWidthMediaFeatureEval},
    {u"display-mode", DisplayModeMediaFeatureEval},
    {u"display-state", DisplayStateMediaFeatureEval},
    {u"resizable", ResizableMediaFeatureEval},
    {u"max-color", Prefixed<ColorMediaFeatureEval, Op::kLe>},
    {u"max-aspect-ratio", Prefixed<AspectRatioMediaFeatureEval, Op::kLe>},
    {u"max-device-aspect-ratio",
     Prefixed<DeviceAspectRatioMediaFeatureEval, Op::kLe>},
    {u"-webkit-max-device-pixel-ratio",
     Prefixed<DevicePixelRatioMediaFeatureEval, Op::kLe>},
    {u"max-device-height", Prefixed<DeviceHeightMediaFeatureEval, Op::kLe>},
    {u"max-device-width", Prefixed<DeviceWidthMediaFeatureEval, Op::kLe>},
    {u"max-height", Prefixed<HeightMediaFeatureEval, Op::kLe>},
    {u"max-monochrome", Prefixed<MonochromeMediaFeatureEval, Op::kLe>},
    {u"max-width", Prefixed<WidthMediaFeatureEval, Op::kLe>},
    {u"max-resolution", Prefixed<ResolutionMediaFeatureEval, Op::kLe>},
    {u"min-color", Prefixed<ColorMediaFeatureEval, Op::kGe>},
    {u"min-aspect-ratio", Prefixed<AspectRatioMediaFeatureEval, Op::kGe>},
    {u"min-device-aspect-ratio",
     Prefixed<DeviceAspectRatioMediaFeatureEval, Op::kGe>},
    {u"-webkit-min-device-pixel-ratio",
     Prefixed<DevicePixelRatioMediaFeatureEval, Op::kGe>},
    {u"min-device-height", Prefixed<DeviceHeightMediaFeatureEval, Op::kGe>},
    {u"min-device-width", Prefixed<DeviceWidthMediaFeatureEval, Op::kGe>},
    {u"min-height", Prefixed<HeightMediaFeatureEval, Op::kGe>},
    {u"min-monochrome", Prefixed<MonochromeMediaFeatureEval, Op::kGe>},
    {u"min-width", Prefixed<WidthMediaFeatureEval, Op::kGe>},
    {u"min-resolution", Prefixed<ResolutionMediaFeatureEval, Op::kGe>},
    {u"pointer", PointerMediaFeatureEval},
    {u"any-pointer", AnyPointerMediaFeatureEval},
    {u"any-hover", AnyHoverMediaFeatureEval},
    {u"-webkit-transform-3d", Transform3dMediaFeatureEval},
    {u"scan", ScanMediaFeatureEval},
    {u"color-index", ColorIndexMediaFeatureEval},
    {u"max-color-index", Prefixed<ColorIndexMediaFeatureEval, Op::kLe>},
    {u"min-color-index", Prefixed<ColorIndexMediaFeatureEval, Op::kGe>},
    {u"resolution", ResolutionMediaFeatureEval},
    {u"prefers-color-scheme", PrefersColorSchemeMediaFeatureEval},
    {u"prefers-contrast", PrefersContrastMediaFeatureEval},
    {u"prefers-reduced-motion", PrefersReducedMotionMediaFeatureEval},
    {u"prefers-reduced-data", PrefersReducedDataMediaFeatureEval},
    {u"prefers-reduced-transparency",
     PrefersReducedTransparencyMediaFeatureEval},
    {u"forced-colors", ForcedColorsMediaFeatureEval},
    {u"navigation-controls", NavigationControlsMediaFeatureEval},
    {u"horizontal-viewport-segments",
     HorizontalViewportSegmentsMediaFeatureEval},
    {u"vertical-viewport-segments", VerticalViewportSegmentsMediaFeatureEval},
    {u"overflow-inline", OverflowInlineMediaFeatureEval},
    {u"overflow-block", OverflowBlockMediaFeatureEval},
    {u"device-posture", DevicePostureMediaFeatureEval},
    {u"update", UpdateMediaFeatureEval},
    {u"inverted-colors", InvertedColorsMediaFeatureEval},
    {u"scripting", ScriptingMediaFeatureEval},
    {u"dynamic-range", DynamicRangeMediaFeatureEval},
    // Chrome makes no distinction between the two.
    {u"video-dynamic-range", DynamicRangeMediaFeatureEval},
};

Op ReverseOperator(Op op) {
  switch (op) {
    case Op::kNone:
    case Op::kEq:
      return op;
    case Op::kLt:
      return Op::kGt;
    case Op::kLe:
      return Op::kGe;
    case Op::kGt:
      return Op::kLt;
    case Op::kGe:
      return Op::kLe;
  }
  return op;
}

KleeneValue ApplyRestrictor(MediaQuery::RestrictorType restrictor,
                            KleeneValue value) {
  if (restrictor == MediaQuery::RestrictorType::kNot &&
      value != KleeneValue::kUnknown) {
    return value == KleeneValue::kFalse ? KleeneValue::kTrue
                                        : KleeneValue::kFalse;
  }
  return value;
}

}  // namespace

MediaQueryEvaluator::MediaQueryEvaluator(const MediaValues& media_values)
    : media_values_(media_values) {}

MediaQueryEvaluator::~MediaQueryEvaluator() = default;

bool MediaQueryEvaluator::MediaTypeMatch(
    const std::u16string& media_type) const {
  return media_type.empty() ||
         base::EqualsCaseInsensitiveASCII(media_type, u"all") ||
         base::EqualsCaseInsensitiveASCII(
             media_type, base::ASCIIToUTF16(media_values_->media_type));
}

KleeneValue MediaQueryEvaluator::Eval(const MediaQuery& query) const {
  if (!MediaTypeMatch(query.MediaType())) {
    return ApplyRestrictor(query.Restrictor(), KleeneValue::kFalse);
  }
  if (!query.ExpNode()) {
    return ApplyRestrictor(query.Restrictor(), KleeneValue::kTrue);
  }
  return ApplyRestrictor(query.Restrictor(), Eval(*query.ExpNode()));
}

bool MediaQueryEvaluator::Eval(const MediaQuerySet& query_set) const {
  const auto& queries = query_set.Queries();
  if (queries.empty()) {
    return true;
  }
  KleeneValue result = KleeneValue::kFalse;
  for (size_t i = 0; i < queries.size() && result != KleeneValue::kTrue; ++i) {
    result = Eval(*queries[i]);
  }
  return result == KleeneValue::kTrue;
}

KleeneValue MediaQueryEvaluator::Eval(const ConditionalExpNode& node) const {
  class Visitor final : public ConditionalExpNodeVisitor {
   public:
    explicit Visitor(const MediaQueryEvaluator& evaluator)
        : evaluator_(evaluator) {}

    KleeneValue EvaluateFeature(const MediaQueryExp& exp) override {
      return evaluator_->EvalFeature(exp);
    }

   private:
    const raw_ref<const MediaQueryEvaluator> evaluator_;
  };
  Visitor visitor(*this);
  return node.Evaluate(visitor);
}

KleeneValue MediaQueryEvaluator::EvalFeature(const MediaQueryExp& exp) const {
  EvalFunc func = nullptr;
  for (const FeatureFunction& entry : kFeatureFunctions) {
    if (entry.name == exp.feature) {
      func = entry.func;
      break;
    }
  }
  if (!func) {
    return KleeneValue::kFalse;
  }
  const MediaQueryExpBounds& bounds = exp.bounds;
  bool result = true;
  if (!bounds.IsRange() || bounds.right.IsValid()) {
    result &= func(bounds.right.value, bounds.right.op, *media_values_);
  }
  if (bounds.left.IsValid()) {
    result &= func(bounds.left.value, ReverseOperator(bounds.left.op),
                   *media_values_);
  }
  return result ? KleeneValue::kTrue : KleeneValue::kFalse;
}

}  // namespace mitto
