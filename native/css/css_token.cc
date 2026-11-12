#include "mitto/css/css_token.h"

#include <array>
#include <numbers>

#include "base/strings/string_util.h"

namespace mitto {

namespace {

struct UnitName {
  std::string_view name;
  CssUnit unit;
};

// Upstream: third_party/blink/renderer/core/css/css_primitive_value_units.json5
constexpr auto kUnitNames = std::to_array<UnitName>({
    {"em", CssUnit::kEms},
    {"ex", CssUnit::kExs},
    {"px", CssUnit::kPixels},
    {"cm", CssUnit::kCentimeters},
    {"mm", CssUnit::kMillimeters},
    {"q", CssUnit::kQuarterMillimeters},
    {"in", CssUnit::kInches},
    {"pt", CssUnit::kPoints},
    {"pc", CssUnit::kPicas},
    {"deg", CssUnit::kDegrees},
    {"rad", CssUnit::kRadians},
    {"grad", CssUnit::kGradians},
    {"ms", CssUnit::kMilliseconds},
    {"s", CssUnit::kSeconds},
    {"hz", CssUnit::kHertz},
    {"khz", CssUnit::kKilohertz},
    {"dpi", CssUnit::kDotsPerInch},
    {"dpcm", CssUnit::kDotsPerCentimeter},
    {"dppx", CssUnit::kDotsPerPixel},
    {"x", CssUnit::kX},
    {"vw", CssUnit::kViewportWidth},
    {"vh", CssUnit::kViewportHeight},
    {"vi", CssUnit::kViewportInlineSize},
    {"vb", CssUnit::kViewportBlockSize},
    {"vmin", CssUnit::kViewportMin},
    {"vmax", CssUnit::kViewportMax},
    {"svw", CssUnit::kSmallViewportWidth},
    {"svh", CssUnit::kSmallViewportHeight},
    {"svi", CssUnit::kSmallViewportInlineSize},
    {"svb", CssUnit::kSmallViewportBlockSize},
    {"svmin", CssUnit::kSmallViewportMin},
    {"svmax", CssUnit::kSmallViewportMax},
    {"lvw", CssUnit::kLargeViewportWidth},
    {"lvh", CssUnit::kLargeViewportHeight},
    {"lvi", CssUnit::kLargeViewportInlineSize},
    {"lvb", CssUnit::kLargeViewportBlockSize},
    {"lvmin", CssUnit::kLargeViewportMin},
    {"lvmax", CssUnit::kLargeViewportMax},
    {"dvw", CssUnit::kDynamicViewportWidth},
    {"dvh", CssUnit::kDynamicViewportHeight},
    {"dvi", CssUnit::kDynamicViewportInlineSize},
    {"dvb", CssUnit::kDynamicViewportBlockSize},
    {"dvmin", CssUnit::kDynamicViewportMin},
    {"dvmax", CssUnit::kDynamicViewportMax},
    {"cqw", CssUnit::kContainerWidth},
    {"cqh", CssUnit::kContainerHeight},
    {"cqi", CssUnit::kContainerInlineSize},
    {"cqb", CssUnit::kContainerBlockSize},
    {"cqmin", CssUnit::kContainerMin},
    {"cqmax", CssUnit::kContainerMax},
    {"rem", CssUnit::kRems},
    {"rex", CssUnit::kRexs},
    {"rch", CssUnit::kRchs},
    {"ric", CssUnit::kRics},
    {"fr", CssUnit::kFlex},
    {"turn", CssUnit::kTurns},
    {"ch", CssUnit::kChs},
    {"ic", CssUnit::kIcs},
    {"lh", CssUnit::kLhs},
    {"rlh", CssUnit::kRlhs},
    {"cap", CssUnit::kCaps},
    {"rcap", CssUnit::kRcaps},
    {"__qem", CssUnit::kQuirkyEms},
});

// Upstream: third_party/blink/renderer/core/css/css_resolution_units.h
constexpr double kCssPixelsPerInch = 96;
constexpr double kCssPixelsPerCentimeter = kCssPixelsPerInch / 2.54;
constexpr double kCssPixelsPerMillimeter = kCssPixelsPerCentimeter / 10;
constexpr double kCssPixelsPerQuarterMillimeter = kCssPixelsPerCentimeter / 40;
constexpr double kCssPixelsPerPoint = kCssPixelsPerInch / 72;
constexpr double kCssPixelsPerPica = kCssPixelsPerInch / 6;

}  // namespace

CssToken::CssToken() = default;
CssToken::CssToken(const CssToken&) = default;
CssToken::CssToken(CssToken&&) = default;
CssToken& CssToken::operator=(const CssToken&) = default;
CssToken& CssToken::operator=(CssToken&&) = default;
CssToken::~CssToken() = default;

// Upstream: third_party/blink/renderer/core/css/css_primitive_value.cc
CssUnit CssUnitFromName(std::u16string_view name) {
  for (const UnitName& entry : kUnitNames) {
    if (name.size() != entry.name.size()) {
      continue;
    }
    bool equal = true;
    for (size_t i = 0; i < name.size(); ++i) {
      if (base::ToLowerASCII(name[i]) != entry.name[i]) {
        equal = false;
        break;
      }
    }
    if (equal) {
      return entry.unit;
    }
  }
  return CssUnit::kUnknown;
}

std::string_view CssUnitName(CssUnit unit) {
  switch (unit) {
    case CssUnit::kUnknown:
    case CssUnit::kNumber:
    case CssUnit::kInteger:
      return "";
    case CssUnit::kPercentage:
      return "%";
    case CssUnit::kQuirkyEms:
      return "em";
    default:
      break;
  }
  for (const UnitName& entry : kUnitNames) {
    if (entry.unit == unit) {
      return entry.name;
    }
  }
  return "";
}

// Upstream: third_party/blink/renderer/core/css/css_primitive_value.cc
CssUnitCategory CssUnitCategoryOf(CssUnit unit) {
  switch (unit) {
    case CssUnit::kNumber:
    case CssUnit::kInteger:
      return CssUnitCategory::kNumber;
    case CssUnit::kPercentage:
      return CssUnitCategory::kPercent;
    case CssUnit::kPixels:
    case CssUnit::kCentimeters:
    case CssUnit::kMillimeters:
    case CssUnit::kQuarterMillimeters:
    case CssUnit::kInches:
    case CssUnit::kPoints:
    case CssUnit::kPicas:
      return CssUnitCategory::kLength;
    case CssUnit::kMilliseconds:
    case CssUnit::kSeconds:
      return CssUnitCategory::kTime;
    case CssUnit::kDegrees:
    case CssUnit::kRadians:
    case CssUnit::kGradians:
    case CssUnit::kTurns:
      return CssUnitCategory::kAngle;
    case CssUnit::kHertz:
    case CssUnit::kKilohertz:
      return CssUnitCategory::kFrequency;
    case CssUnit::kDotsPerPixel:
    case CssUnit::kX:
    case CssUnit::kDotsPerInch:
    case CssUnit::kDotsPerCentimeter:
      return CssUnitCategory::kResolution;
    default:
      return CssUnitCategory::kOther;
  }
}

CssUnit CanonicalUnitFor(CssUnitCategory category) {
  switch (category) {
    case CssUnitCategory::kNumber:
      return CssUnit::kNumber;
    case CssUnitCategory::kLength:
      return CssUnit::kPixels;
    case CssUnitCategory::kTime:
      return CssUnit::kSeconds;
    case CssUnitCategory::kAngle:
      return CssUnit::kDegrees;
    case CssUnitCategory::kFrequency:
      return CssUnit::kHertz;
    case CssUnitCategory::kResolution:
      return CssUnit::kDotsPerPixel;
    case CssUnitCategory::kPercent:
    case CssUnitCategory::kOther:
      return CssUnit::kUnknown;
  }
  return CssUnit::kUnknown;
}

double ConversionToCanonicalUnits(CssUnit unit) {
  switch (unit) {
    case CssUnit::kMilliseconds:
      return 0.001;
    case CssUnit::kCentimeters:
      return kCssPixelsPerCentimeter;
    case CssUnit::kDotsPerCentimeter:
      return 1 / kCssPixelsPerCentimeter;
    case CssUnit::kMillimeters:
      return kCssPixelsPerMillimeter;
    case CssUnit::kQuarterMillimeters:
      return kCssPixelsPerQuarterMillimeter;
    case CssUnit::kInches:
      return kCssPixelsPerInch;
    case CssUnit::kDotsPerInch:
      return 1 / kCssPixelsPerInch;
    case CssUnit::kPoints:
      return kCssPixelsPerPoint;
    case CssUnit::kPicas:
      return kCssPixelsPerPica;
    case CssUnit::kRadians:
      return 180 / std::numbers::pi;
    case CssUnit::kGradians:
      return 0.9;
    case CssUnit::kTurns:
      return 360;
    case CssUnit::kKilohertz:
      return 1000;
    default:
      return 1;
  }
}

bool IsLengthUnit(CssUnit unit) {
  return CssUnitCategoryOf(unit) == CssUnitCategory::kLength ||
         IsRelativeLengthUnit(unit);
}

bool IsRelativeLengthUnit(CssUnit unit) {
  switch (unit) {
    case CssUnit::kEms:
    case CssUnit::kExs:
    case CssUnit::kRems:
    case CssUnit::kChs:
    case CssUnit::kRexs:
    case CssUnit::kRchs:
    case CssUnit::kRics:
    case CssUnit::kIcs:
    case CssUnit::kLhs:
    case CssUnit::kRlhs:
    case CssUnit::kCaps:
    case CssUnit::kRcaps:
    case CssUnit::kQuirkyEms:
    case CssUnit::kViewportWidth:
    case CssUnit::kViewportHeight:
    case CssUnit::kViewportInlineSize:
    case CssUnit::kViewportBlockSize:
    case CssUnit::kViewportMin:
    case CssUnit::kViewportMax:
    case CssUnit::kSmallViewportWidth:
    case CssUnit::kSmallViewportHeight:
    case CssUnit::kSmallViewportInlineSize:
    case CssUnit::kSmallViewportBlockSize:
    case CssUnit::kSmallViewportMin:
    case CssUnit::kSmallViewportMax:
    case CssUnit::kLargeViewportWidth:
    case CssUnit::kLargeViewportHeight:
    case CssUnit::kLargeViewportInlineSize:
    case CssUnit::kLargeViewportBlockSize:
    case CssUnit::kLargeViewportMin:
    case CssUnit::kLargeViewportMax:
    case CssUnit::kDynamicViewportWidth:
    case CssUnit::kDynamicViewportHeight:
    case CssUnit::kDynamicViewportInlineSize:
    case CssUnit::kDynamicViewportBlockSize:
    case CssUnit::kDynamicViewportMin:
    case CssUnit::kDynamicViewportMax:
    case CssUnit::kContainerWidth:
    case CssUnit::kContainerHeight:
    case CssUnit::kContainerInlineSize:
    case CssUnit::kContainerBlockSize:
    case CssUnit::kContainerMin:
    case CssUnit::kContainerMax:
      return true;
    default:
      return false;
  }
}

bool IsResolutionUnit(CssUnit unit) {
  return CssUnitCategoryOf(unit) == CssUnitCategory::kResolution;
}

}  // namespace mitto
