// What a media query's features read of the page, its window and its
// machine, as Blink's MediaValues gives it to the evaluator. Blink reads a
// frame's view and settings; a Page fills this from its start message, which
// carries the Session's Machine and Persona, and a test fills it as Blink's
// tests fill MediaValuesCachedData, whose defaults these are.
//
// Upstream: third_party/blink/renderer/core/css/media_values.h
// Upstream: third_party/blink/renderer/core/css/media_values_cached.h

#ifndef MITTO_CSS_MEDIA_VALUES_H_
#define MITTO_CSS_MEDIA_VALUES_H_

#include <string>

#include "mitto/css/css_math_expression.h"

namespace mitto {

// Bits of ui's pointer and hover types, as Blink's mojom enums have them.
inline constexpr int kPointerTypeNone = 1 << 0;
inline constexpr int kPointerTypeCoarse = 1 << 1;
inline constexpr int kPointerTypeFine = 1 << 2;
inline constexpr int kHoverTypeNone = 1 << 0;
inline constexpr int kHoverTypeHover = 1 << 1;

enum class MediaDisplayMode {
  kBrowser,
  kMinimalUi,
  kStandalone,
  kFullscreen,
  kWindowControlsOverlay,
  kTabbed,
  kPictureInPicture,
  kUnframed,
};

enum class MediaWindowShowState {
  kDefault,
  kNormal,
  kMinimized,
  kMaximized,
  kInactive,
  kFullscreen,
};

enum class MediaColorGamut {
  kSrgb,
  kP3,
  kBt2020,
};

enum class MediaColorScheme {
  kLight,
  kDark,
};

enum class MediaContrast {
  kNoPreference,
  kMore,
  kLess,
  kCustom,
};

enum class MediaUpdate {
  kSlow,
  kFast,
};

enum class MediaScripting {
  kNone,
  kInitialOnly,
  kEnabled,
};

enum class MediaDevicePosture {
  kContinuous,
  kFolded,
};

struct MediaValues {
  MediaValues();
  MediaValues(const MediaValues&);
  MediaValues& operator=(const MediaValues&);
  ~MediaValues();

  // The viewport, in CSS pixels, and what relative lengths resolve against:
  // its size and the root element's font.
  double viewport_width = 0;
  double viewport_height = 0;
  // The screen, in CSS pixels.
  int device_width = 0;
  int device_height = 0;
  float device_pixel_ratio = 1;
  bool device_supports_hdr = false;
  int color_bits_per_component = 24;
  int monochrome_bits_per_component = 0;
  bool inverted_colors = false;
  int primary_pointer_type = kPointerTypeNone;
  int available_pointer_types = kPointerTypeNone;
  int primary_hover_type = kHoverTypeNone;
  int available_hover_types = kHoverTypeNone;
  MediaUpdate update = MediaUpdate::kFast;
  // The font sizes that em, ex, ch, ic, cap and lh, and their root forms,
  // resolve against.
  float em_size = 16;
  float ex_size = 8;
  float ch_size = 8;
  float ic_size = 16;
  float cap_size = 16;
  float line_height = 0;
  bool three_d_enabled = false;
  // Whether the document is in no-quirks mode, which takes a number for a
  // length only if it is 0.
  bool strict_mode = true;
  // Lower case, such as "screen". Empty matches only "all".
  std::string media_type;
  MediaDisplayMode display_mode = MediaDisplayMode::kBrowser;
  MediaWindowShowState window_show_state = MediaWindowShowState::kDefault;
  bool resizable = true;
  MediaColorGamut color_gamut = MediaColorGamut::kSrgb;
  MediaColorScheme preferred_color_scheme = MediaColorScheme::kLight;
  MediaContrast preferred_contrast = MediaContrast::kNoPreference;
  bool prefers_reduced_motion = false;
  bool prefers_reduced_data = false;
  bool prefers_reduced_transparency = false;
  bool forced_colors = false;
  bool navigation_controls_back_button = false;
  int horizontal_viewport_segments = 0;
  int vertical_viewport_segments = 0;
  MediaDevicePosture device_posture = MediaDevicePosture::kContinuous;
  MediaScripting scripting = MediaScripting::kNone;

  CssLengthResolver LengthResolver() const;
};

}  // namespace mitto

#endif  // MITTO_CSS_MEDIA_VALUES_H_
