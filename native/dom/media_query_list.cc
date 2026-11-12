#include "mitto/dom/media_query_list.h"

#include <optional>
#include <utility>

#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_media_query_list.h"
#include "mitto/css/media_query_evaluator.h"
#include "mitto/css/media_values.h"
#include "mitto/dom/document.h"
#include "mitto/dom/window.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

namespace {

constexpr char16_t kChange[] = u"change";

// Chrome's screen_info.depth_per_component for a display of 8 bits a channel,
// which a Machine has: it holds no wide-gamut or HDR display.
constexpr int kColorBitsPerComponent = 8;

}  // namespace

// What Chrome on Windows gives each media feature for the Session's Machine,
// in a tab of a normal window.
// Upstream: third_party/blink/renderer/core/css/media_values_dynamic.cc
// Upstream: third_party/blink/renderer/core/css/media_values.cc
// Upstream: ui/base/pointer/pointer_device_win.cc
// Upstream: ui/native_theme/os_settings_provider.cc
MediaValues MediaValuesFor(const PageStart& start) {
  MediaValues values;
  values.media_type = "screen";
  values.viewport_width = start.viewport_width;
  values.viewport_height = start.viewport_height;
  values.device_width = start.screen_width;
  values.device_height = start.screen_height;
  // The scale times the zoom, which a Machine holds at 100%.
  values.device_pixel_ratio = static_cast<float>(start.scale);
  values.device_supports_hdr = false;
  values.color_bits_per_component = kColorBitsPerComponent;
  values.monochrome_bits_per_component = 0;
  values.color_gamut = MediaColorGamut::kSrgb;
  values.inverted_colors = start.inverted_colors;
  // A desktop always has a mouse, which Windows reports even for a
  // touchpad, and a touch screen adds a coarse pointer. A tablet with no
  // keyboard would have only the coarse one; a Machine is never one.
  values.primary_pointer_type = kPointerTypeFine;
  values.available_pointer_types =
      kPointerTypeFine | (start.touch_points > 0 ? kPointerTypeCoarse : 0);
  values.primary_hover_type = kHoverTypeHover;
  values.available_hover_types = kHoverTypeHover;
  values.update = MediaUpdate::kFast;
  values.three_d_enabled = true;
  values.display_mode = MediaDisplayMode::kBrowser;
  values.window_show_state = MediaWindowShowState::kDefault;
  values.resizable = true;
  values.preferred_color_scheme = start.color_scheme == "dark"
                                      ? MediaColorScheme::kDark
                                      : MediaColorScheme::kLight;
  // Every contrast theme that a Machine can have has a contrast of at least
  // 7:1 between its Window and WindowText colours (docs/persona.md).
  const bool contrast_theme =
      !start.contrast_theme.empty() && start.contrast_theme != "none";
  values.forced_colors = contrast_theme;
  values.preferred_contrast =
      contrast_theme ? MediaContrast::kMore : MediaContrast::kNoPreference;
  values.prefers_reduced_motion = start.reduced_motion == "reduce";
  values.prefers_reduced_transparency = start.reduced_transparency == "reduce";
  values.prefers_reduced_data = false;
  values.navigation_controls_back_button = false;
  // A desktop window is one segment.
  values.horizontal_viewport_segments = 1;
  values.vertical_viewport_segments = 1;
  values.device_posture = MediaDevicePosture::kContinuous;
  values.scripting = MediaScripting::kEnabled;
  return values;
}

MediaQueryFeatures MediaQueryFeaturesFor(
    const bindings::RuntimeFeatureSet& features) {
  using bindings::RuntimeFeature;
  MediaQueryFeatures result;
  result.forced_colors = features.IsEnabled(RuntimeFeature::kForcedColors);
  result.inverted_colors = features.IsEnabled(RuntimeFeature::kInvertedColors);
  result.prefers_reduced_data =
      features.IsEnabled(RuntimeFeature::kPrefersReducedData);
  result.video_dynamic_range =
      features.IsEnabled(RuntimeFeature::kCSSVideoDynamicRangeMediaQueries);
  result.navigation_controls =
      features.IsEnabled(RuntimeFeature::kMediaQueryNavigationControls);
  result.viewport_segments =
      features.IsEnabled(RuntimeFeature::kViewportSegments);
  result.device_posture = features.IsEnabled(RuntimeFeature::kDevicePosture);
  result.additional_windowing_controls = features.IsEnabled(
      RuntimeFeature::kDesktopPWAsAdditionalWindowingControls);
  result.scrolled_container_queries =
      features.IsEnabled(RuntimeFeature::kCSSScrolledContainerQueries);
  return result;
}

// As Blink's MediaQueryMatcher::MatchMedia().
// static
MediaQueryList* MediaQueryList::Create(Window* window,
                                       const std::u16string& query) {
  const MediaQueryFeatures features = MediaQueryFeaturesFor(
      bindings::PerIsolateData::From(v8::Isolate::GetCurrent())->features());
  return bindings::MakeGarbageCollected<MediaQueryList>(
      window, ParseMediaQuerySet(query, features));
}

MediaQueryList::MediaQueryList(Window* window,
                               std::unique_ptr<MediaQuerySet> media)
    : window_(window), media_(std::move(media)) {}

MediaQueryList::~MediaQueryList() = default;

const bindings::WrapperTypeInfo* MediaQueryList::GetWrapperTypeInfo() const {
  return &bindings::V8MediaQueryList::kWrapperTypeInfo;
}

void MediaQueryList::Trace(cppgc::Visitor* visitor) const {
  EventTarget::Trace(visitor);
  visitor->Trace(window_);
}

std::u16string MediaQueryList::Media() const {
  return media_->MediaText();
}

// Evaluated on each read, as Blink's UpdateMatches() does, with what the
// window has now: its machine never changes, but its document's mode can.
bool MediaQueryList::Matches() const {
  MediaValues values = MediaValuesFor(window_->Start());
  values.strict_mode = !window_->GetDocument().InQuirksMode();
  return MediaQueryEvaluator(values).Eval(*media_);
}

void MediaQueryList::AddDeprecatedListener(v8::Local<v8::Object> listener) {
  AddEventListener(kChange, listener, std::nullopt);
}

void MediaQueryList::RemoveDeprecatedListener(v8::Local<v8::Object> listener) {
  RemoveEventListener(kChange, listener, std::nullopt);
}

}  // namespace mitto
