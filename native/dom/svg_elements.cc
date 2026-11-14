#include "mitto/dom/svg_elements.h"

#include <string_view>

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_svg_a_element.h"
#include "mitto/bindings/v8_svg_animate_element.h"
#include "mitto/bindings/v8_svg_animate_motion_element.h"
#include "mitto/bindings/v8_svg_animate_transform_element.h"
#include "mitto/bindings/v8_svg_animation_element.h"
#include "mitto/bindings/v8_svg_circle_element.h"
#include "mitto/bindings/v8_svg_clip_path_element.h"
#include "mitto/bindings/v8_svg_component_transfer_function_element.h"
#include "mitto/bindings/v8_svg_defs_element.h"
#include "mitto/bindings/v8_svg_desc_element.h"
#include "mitto/bindings/v8_svg_ellipse_element.h"
#include "mitto/bindings/v8_svg_fe_blend_element.h"
#include "mitto/bindings/v8_svg_fe_color_matrix_element.h"
#include "mitto/bindings/v8_svg_fe_component_transfer_element.h"
#include "mitto/bindings/v8_svg_fe_composite_element.h"
#include "mitto/bindings/v8_svg_fe_convolve_matrix_element.h"
#include "mitto/bindings/v8_svg_fe_diffuse_lighting_element.h"
#include "mitto/bindings/v8_svg_fe_displacement_map_element.h"
#include "mitto/bindings/v8_svg_fe_distant_light_element.h"
#include "mitto/bindings/v8_svg_fe_drop_shadow_element.h"
#include "mitto/bindings/v8_svg_fe_flood_element.h"
#include "mitto/bindings/v8_svg_fe_func_a_element.h"
#include "mitto/bindings/v8_svg_fe_func_b_element.h"
#include "mitto/bindings/v8_svg_fe_func_g_element.h"
#include "mitto/bindings/v8_svg_fe_func_r_element.h"
#include "mitto/bindings/v8_svg_fe_gaussian_blur_element.h"
#include "mitto/bindings/v8_svg_fe_image_element.h"
#include "mitto/bindings/v8_svg_fe_merge_element.h"
#include "mitto/bindings/v8_svg_fe_merge_node_element.h"
#include "mitto/bindings/v8_svg_fe_morphology_element.h"
#include "mitto/bindings/v8_svg_fe_offset_element.h"
#include "mitto/bindings/v8_svg_fe_point_light_element.h"
#include "mitto/bindings/v8_svg_fe_specular_lighting_element.h"
#include "mitto/bindings/v8_svg_fe_spot_light_element.h"
#include "mitto/bindings/v8_svg_fe_tile_element.h"
#include "mitto/bindings/v8_svg_fe_turbulence_element.h"
#include "mitto/bindings/v8_svg_filter_element.h"
#include "mitto/bindings/v8_svg_foreign_object_element.h"
#include "mitto/bindings/v8_svg_g_element.h"
#include "mitto/bindings/v8_svg_geometry_element.h"
#include "mitto/bindings/v8_svg_gradient_element.h"
#include "mitto/bindings/v8_svg_graphics_element.h"
#include "mitto/bindings/v8_svg_image_element.h"
#include "mitto/bindings/v8_svg_line_element.h"
#include "mitto/bindings/v8_svg_linear_gradient_element.h"
#include "mitto/bindings/v8_svg_marker_element.h"
#include "mitto/bindings/v8_svg_mask_element.h"
#include "mitto/bindings/v8_svg_metadata_element.h"
#include "mitto/bindings/v8_svg_mpath_element.h"
#include "mitto/bindings/v8_svg_path_element.h"
#include "mitto/bindings/v8_svg_pattern_element.h"
#include "mitto/bindings/v8_svg_polygon_element.h"
#include "mitto/bindings/v8_svg_polyline_element.h"
#include "mitto/bindings/v8_svg_radial_gradient_element.h"
#include "mitto/bindings/v8_svg_rect_element.h"
#include "mitto/bindings/v8_svg_script_element.h"
#include "mitto/bindings/v8_svg_set_element.h"
#include "mitto/bindings/v8_svg_stop_element.h"
#include "mitto/bindings/v8_svg_style_element.h"
#include "mitto/bindings/v8_svg_svg_element.h"
#include "mitto/bindings/v8_svg_switch_element.h"
#include "mitto/bindings/v8_svg_symbol_element.h"
#include "mitto/bindings/v8_svg_text_content_element.h"
#include "mitto/bindings/v8_svg_text_element.h"
#include "mitto/bindings/v8_svg_text_path_element.h"
#include "mitto/bindings/v8_svg_text_positioning_element.h"
#include "mitto/bindings/v8_svg_title_element.h"
#include "mitto/bindings/v8_svg_tspan_element.h"
#include "mitto/bindings/v8_svg_use_element.h"
#include "mitto/bindings/v8_svg_view_element.h"

namespace mitto {

namespace {

// The keywords of Blink's GetEnumerationMap<>()s, with the largest value that
// a page can see.
constexpr std::u16string_view kUnitTypesKeywords[] = {u"userSpaceOnUse",
                                                      u"objectBoundingBox"};
constexpr SVGEnumerationMap kUnitTypes{kUnitTypesKeywords, 2};
constexpr std::u16string_view kComponentTransferTypesKeywords[] = {
    u"identity", u"table", u"discrete", u"linear", u"gamma"};
constexpr SVGEnumerationMap kComponentTransferTypes{
    kComponentTransferTypesKeywords, 5};
constexpr std::u16string_view kBlendModesKeywords[] = {
    u"normal",     u"multiply",   u"screen",      u"darken",
    u"lighten",    u"overlay",    u"color-dodge", u"color-burn",
    u"hard-light", u"soft-light", u"difference",  u"exclusion",
    u"hue",        u"saturation", u"color",       u"luminosity"};
constexpr SVGEnumerationMap kBlendModes{kBlendModesKeywords, 16};
constexpr std::u16string_view kColorMatrixTypesKeywords[] = {
    u"matrix", u"saturate", u"hueRotate", u"luminanceToAlpha"};
constexpr SVGEnumerationMap kColorMatrixTypes{kColorMatrixTypesKeywords, 4};
constexpr std::u16string_view kCompositeOperatorsKeywords[] = {
    u"over", u"in", u"out", u"atop", u"xor", u"arithmetic", u"lighter"};
constexpr SVGEnumerationMap kCompositeOperators{kCompositeOperatorsKeywords, 6};
constexpr std::u16string_view kEdgeModesKeywords[] = {u"duplicate", u"wrap",
                                                      u"none"};
constexpr SVGEnumerationMap kEdgeModes{kEdgeModesKeywords, 3};
constexpr std::u16string_view kChannelSelectorsKeywords[] = {u"R", u"G", u"B",
                                                             u"A"};
constexpr SVGEnumerationMap kChannelSelectors{kChannelSelectorsKeywords, 4};
constexpr std::u16string_view kMorphologyOperatorsKeywords[] = {u"erode",
                                                                u"dilate"};
constexpr SVGEnumerationMap kMorphologyOperators{kMorphologyOperatorsKeywords,
                                                 2};
constexpr std::u16string_view kStitchTypesKeywords[] = {u"stitch", u"noStitch"};
constexpr SVGEnumerationMap kStitchTypes{kStitchTypesKeywords, 2};
constexpr std::u16string_view kTurbulenceTypesKeywords[] = {u"fractalNoise",
                                                            u"turbulence"};
constexpr SVGEnumerationMap kTurbulenceTypes{kTurbulenceTypesKeywords, 2};
constexpr std::u16string_view kSpreadMethodsKeywords[] = {u"pad", u"reflect",
                                                          u"repeat"};
constexpr SVGEnumerationMap kSpreadMethods{kSpreadMethodsKeywords, 3};
constexpr std::u16string_view kMarkerUnitsKeywords[] = {u"userSpaceOnUse",
                                                        u"strokeWidth"};
constexpr SVGEnumerationMap kMarkerUnits{kMarkerUnitsKeywords, 2};
constexpr std::u16string_view kLengthAdjustsKeywords[] = {u"spacing",
                                                          u"spacingAndGlyphs"};
constexpr SVGEnumerationMap kLengthAdjusts{kLengthAdjustsKeywords, 2};
constexpr std::u16string_view kTextPathMethodsKeywords[] = {u"align",
                                                            u"stretch"};
constexpr SVGEnumerationMap kTextPathMethods{kTextPathMethodsKeywords, 2};
constexpr std::u16string_view kTextPathSpacingsKeywords[] = {u"auto", u"exact"};
constexpr SVGEnumerationMap kTextPathSpacings{kTextPathSpacingsKeywords, 2};
constexpr std::u16string_view kTextPathSidesKeywords[] = {u"left", u"right"};
constexpr SVGEnumerationMap kTextPathSides{kTextPathSidesKeywords, 2};

}  // namespace

SVGAnimationElement::SVGAnimationElement(Document& document,
                                         const std::u16string& local_name)
    : SVGElement(document, local_name) {}

const bindings::WrapperTypeInfo* SVGAnimationElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGAnimationElement::kWrapperTypeInfo;
}

SVGComponentTransferFunctionElement::SVGComponentTransferFunctionElement(
    Document& document,
    const std::u16string& local_name)
    : SVGElement(document, local_name) {}

const bindings::WrapperTypeInfo*
SVGComponentTransferFunctionElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGComponentTransferFunctionElement::kWrapperTypeInfo;
}

SVGAnimatedEnumeration* SVGComponentTransferFunctionElement::Type() {
  return AnimatedEnumeration(u"type", kComponentTransferTypes, 1);
}

SVGAnimatedNumber* SVGComponentTransferFunctionElement::Slope() {
  return AnimatedNumber(u"slope", 1);
}

SVGAnimatedNumber* SVGComponentTransferFunctionElement::Intercept() {
  return AnimatedNumber(u"intercept", 0);
}

SVGAnimatedNumber* SVGComponentTransferFunctionElement::Amplitude() {
  return AnimatedNumber(u"amplitude", 1);
}

SVGAnimatedNumber* SVGComponentTransferFunctionElement::Exponent() {
  return AnimatedNumber(u"exponent", 1);
}

SVGAnimatedNumber* SVGComponentTransferFunctionElement::Offset() {
  return AnimatedNumber(u"offset", 0);
}

SVGGradientElement::SVGGradientElement(Document& document,
                                       const std::u16string& local_name)
    : SVGElement(document, local_name) {}

const bindings::WrapperTypeInfo* SVGGradientElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGGradientElement::kWrapperTypeInfo;
}

SVGAnimatedEnumeration* SVGGradientElement::GradientUnits() {
  return AnimatedEnumeration(u"gradientUnits", kUnitTypes, 2);
}

SVGAnimatedEnumeration* SVGGradientElement::SpreadMethod() {
  return AnimatedEnumeration(u"spreadMethod", kSpreadMethods, 1);
}

SVGGraphicsElement::SVGGraphicsElement(Document& document,
                                       const std::u16string& local_name)
    : SVGElement(document, local_name) {}

const bindings::WrapperTypeInfo* SVGGraphicsElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGGraphicsElement::kWrapperTypeInfo;
}

SVGGeometryElement::SVGGeometryElement(Document& document,
                                       const std::u16string& local_name)
    : SVGGraphicsElement(document, local_name) {}

const bindings::WrapperTypeInfo* SVGGeometryElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGGeometryElement::kWrapperTypeInfo;
}

SVGAnimatedNumber* SVGGeometryElement::PathLength() {
  return AnimatedNumber(u"pathLength", 0);
}

SVGTextContentElement::SVGTextContentElement(Document& document,
                                             const std::u16string& local_name)
    : SVGGraphicsElement(document, local_name) {}

const bindings::WrapperTypeInfo* SVGTextContentElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGTextContentElement::kWrapperTypeInfo;
}

SVGAnimatedEnumeration* SVGTextContentElement::LengthAdjust() {
  return AnimatedEnumeration(u"lengthAdjust", kLengthAdjusts, 1);
}

SVGTextPositioningElement::SVGTextPositioningElement(
    Document& document,
    const std::u16string& local_name)
    : SVGTextContentElement(document, local_name) {}

const bindings::WrapperTypeInfo* SVGTextPositioningElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGTextPositioningElement::kWrapperTypeInfo;
}

// static
SVGAElement* SVGAElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGAElement>(document);
}

SVGAElement::SVGAElement(Document& document)
    : SVGGraphicsElement(document, u"a") {}

const bindings::WrapperTypeInfo* SVGAElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGAElement::kWrapperTypeInfo;
}

SVGAnimatedString* SVGAElement::SvgTarget() {
  return AnimatedString(u"target");
}

int32_t SVGAElement::DefaultTabIndex() const {
  return 0;
}

// static
SVGAnimateElement* SVGAnimateElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGAnimateElement>(document);
}

SVGAnimateElement::SVGAnimateElement(Document& document)
    : SVGAnimationElement(document, u"animate") {}

const bindings::WrapperTypeInfo* SVGAnimateElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGAnimateElement::kWrapperTypeInfo;
}

// static
SVGAnimateMotionElement* SVGAnimateMotionElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGAnimateMotionElement>(document);
}

SVGAnimateMotionElement::SVGAnimateMotionElement(Document& document)
    : SVGAnimationElement(document, u"animateMotion") {}

const bindings::WrapperTypeInfo* SVGAnimateMotionElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGAnimateMotionElement::kWrapperTypeInfo;
}

// static
SVGAnimateTransformElement* SVGAnimateTransformElement::Create(
    Document& document) {
  return bindings::MakeGarbageCollected<SVGAnimateTransformElement>(document);
}

SVGAnimateTransformElement::SVGAnimateTransformElement(Document& document)
    : SVGAnimationElement(document, u"animateTransform") {}

const bindings::WrapperTypeInfo*
SVGAnimateTransformElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGAnimateTransformElement::kWrapperTypeInfo;
}

// static
SVGCircleElement* SVGCircleElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGCircleElement>(document);
}

SVGCircleElement::SVGCircleElement(Document& document)
    : SVGGeometryElement(document, u"circle") {}

const bindings::WrapperTypeInfo* SVGCircleElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGCircleElement::kWrapperTypeInfo;
}

// static
SVGClipPathElement* SVGClipPathElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGClipPathElement>(document);
}

SVGClipPathElement::SVGClipPathElement(Document& document)
    : SVGElement(document, u"clipPath") {}

const bindings::WrapperTypeInfo* SVGClipPathElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGClipPathElement::kWrapperTypeInfo;
}

SVGAnimatedEnumeration* SVGClipPathElement::ClipPathUnits() {
  return AnimatedEnumeration(u"clipPathUnits", kUnitTypes, 1);
}

// static
SVGDefsElement* SVGDefsElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGDefsElement>(document);
}

SVGDefsElement::SVGDefsElement(Document& document)
    : SVGGraphicsElement(document, u"defs") {}

const bindings::WrapperTypeInfo* SVGDefsElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGDefsElement::kWrapperTypeInfo;
}

// static
SVGDescElement* SVGDescElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGDescElement>(document);
}

SVGDescElement::SVGDescElement(Document& document)
    : SVGElement(document, u"desc") {}

const bindings::WrapperTypeInfo* SVGDescElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGDescElement::kWrapperTypeInfo;
}

// static
SVGEllipseElement* SVGEllipseElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGEllipseElement>(document);
}

SVGEllipseElement::SVGEllipseElement(Document& document)
    : SVGGeometryElement(document, u"ellipse") {}

const bindings::WrapperTypeInfo* SVGEllipseElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGEllipseElement::kWrapperTypeInfo;
}

// static
SVGFEBlendElement* SVGFEBlendElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGFEBlendElement>(document);
}

SVGFEBlendElement::SVGFEBlendElement(Document& document)
    : SVGElement(document, u"feBlend") {}

const bindings::WrapperTypeInfo* SVGFEBlendElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGFEBlendElement::kWrapperTypeInfo;
}

SVGAnimatedString* SVGFEBlendElement::In1() {
  return AnimatedString(u"in");
}

SVGAnimatedString* SVGFEBlendElement::In2() {
  return AnimatedString(u"in2");
}

SVGAnimatedEnumeration* SVGFEBlendElement::Mode() {
  return AnimatedEnumeration(u"mode", kBlendModes, 1);
}

// static
SVGFEColorMatrixElement* SVGFEColorMatrixElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGFEColorMatrixElement>(document);
}

SVGFEColorMatrixElement::SVGFEColorMatrixElement(Document& document)
    : SVGElement(document, u"feColorMatrix") {}

const bindings::WrapperTypeInfo* SVGFEColorMatrixElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGFEColorMatrixElement::kWrapperTypeInfo;
}

SVGAnimatedString* SVGFEColorMatrixElement::In1() {
  return AnimatedString(u"in");
}

SVGAnimatedEnumeration* SVGFEColorMatrixElement::Type() {
  return AnimatedEnumeration(u"type", kColorMatrixTypes, 1);
}

// static
SVGFEComponentTransferElement* SVGFEComponentTransferElement::Create(
    Document& document) {
  return bindings::MakeGarbageCollected<SVGFEComponentTransferElement>(
      document);
}

SVGFEComponentTransferElement::SVGFEComponentTransferElement(Document& document)
    : SVGElement(document, u"feComponentTransfer") {}

const bindings::WrapperTypeInfo*
SVGFEComponentTransferElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGFEComponentTransferElement::kWrapperTypeInfo;
}

SVGAnimatedString* SVGFEComponentTransferElement::In1() {
  return AnimatedString(u"in");
}

// static
SVGFECompositeElement* SVGFECompositeElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGFECompositeElement>(document);
}

SVGFECompositeElement::SVGFECompositeElement(Document& document)
    : SVGElement(document, u"feComposite") {}

const bindings::WrapperTypeInfo* SVGFECompositeElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGFECompositeElement::kWrapperTypeInfo;
}

SVGAnimatedString* SVGFECompositeElement::In2() {
  return AnimatedString(u"in2");
}

SVGAnimatedString* SVGFECompositeElement::In1() {
  return AnimatedString(u"in");
}

SVGAnimatedEnumeration* SVGFECompositeElement::SvgOperator() {
  return AnimatedEnumeration(u"operator", kCompositeOperators, 1);
}

SVGAnimatedNumber* SVGFECompositeElement::K1() {
  return AnimatedNumber(u"k1", 0);
}

SVGAnimatedNumber* SVGFECompositeElement::K2() {
  return AnimatedNumber(u"k2", 0);
}

SVGAnimatedNumber* SVGFECompositeElement::K3() {
  return AnimatedNumber(u"k3", 0);
}

SVGAnimatedNumber* SVGFECompositeElement::K4() {
  return AnimatedNumber(u"k4", 0);
}

// static
SVGFEConvolveMatrixElement* SVGFEConvolveMatrixElement::Create(
    Document& document) {
  return bindings::MakeGarbageCollected<SVGFEConvolveMatrixElement>(document);
}

SVGFEConvolveMatrixElement::SVGFEConvolveMatrixElement(Document& document)
    : SVGElement(document, u"feConvolveMatrix") {}

const bindings::WrapperTypeInfo*
SVGFEConvolveMatrixElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGFEConvolveMatrixElement::kWrapperTypeInfo;
}

SVGAnimatedString* SVGFEConvolveMatrixElement::In1() {
  return AnimatedString(u"in");
}

SVGAnimatedInteger* SVGFEConvolveMatrixElement::OrderX() {
  return AnimatedOrder(SVGPairHalf::kFirst);
}

SVGAnimatedInteger* SVGFEConvolveMatrixElement::OrderY() {
  return AnimatedOrder(SVGPairHalf::kSecond);
}

SVGAnimatedNumber* SVGFEConvolveMatrixElement::Divisor() {
  return AnimatedNumber(u"divisor", 1);
}

SVGAnimatedNumber* SVGFEConvolveMatrixElement::Bias() {
  return AnimatedNumber(u"bias", 0);
}

SVGAnimatedInteger* SVGFEConvolveMatrixElement::TargetX() {
  return AnimatedInteger(u"targetX", 0);
}

SVGAnimatedInteger* SVGFEConvolveMatrixElement::TargetY() {
  return AnimatedInteger(u"targetY", 0);
}

SVGAnimatedEnumeration* SVGFEConvolveMatrixElement::EdgeMode() {
  return AnimatedEnumeration(u"edgeMode", kEdgeModes, 1);
}

SVGAnimatedNumber* SVGFEConvolveMatrixElement::KernelUnitLengthX() {
  return AnimatedNumberHalf(u"kernelUnitLength", SVGPairHalf::kFirst, 0);
}

SVGAnimatedNumber* SVGFEConvolveMatrixElement::KernelUnitLengthY() {
  return AnimatedNumberHalf(u"kernelUnitLength", SVGPairHalf::kSecond, 0);
}

SVGAnimatedBoolean* SVGFEConvolveMatrixElement::PreserveAlpha() {
  return AnimatedBoolean(u"preserveAlpha");
}

// static
SVGFEDiffuseLightingElement* SVGFEDiffuseLightingElement::Create(
    Document& document) {
  return bindings::MakeGarbageCollected<SVGFEDiffuseLightingElement>(document);
}

SVGFEDiffuseLightingElement::SVGFEDiffuseLightingElement(Document& document)
    : SVGElement(document, u"feDiffuseLighting") {}

const bindings::WrapperTypeInfo*
SVGFEDiffuseLightingElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGFEDiffuseLightingElement::kWrapperTypeInfo;
}

SVGAnimatedString* SVGFEDiffuseLightingElement::In1() {
  return AnimatedString(u"in");
}

SVGAnimatedNumber* SVGFEDiffuseLightingElement::SurfaceScale() {
  return AnimatedNumber(u"surfaceScale", 1);
}

SVGAnimatedNumber* SVGFEDiffuseLightingElement::DiffuseConstant() {
  return AnimatedNumber(u"diffuseConstant", 1);
}

SVGAnimatedNumber* SVGFEDiffuseLightingElement::KernelUnitLengthX() {
  return AnimatedNumberHalf(u"kernelUnitLength", SVGPairHalf::kFirst, 0);
}

SVGAnimatedNumber* SVGFEDiffuseLightingElement::KernelUnitLengthY() {
  return AnimatedNumberHalf(u"kernelUnitLength", SVGPairHalf::kSecond, 0);
}

// static
SVGFEDisplacementMapElement* SVGFEDisplacementMapElement::Create(
    Document& document) {
  return bindings::MakeGarbageCollected<SVGFEDisplacementMapElement>(document);
}

SVGFEDisplacementMapElement::SVGFEDisplacementMapElement(Document& document)
    : SVGElement(document, u"feDisplacementMap") {}

const bindings::WrapperTypeInfo*
SVGFEDisplacementMapElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGFEDisplacementMapElement::kWrapperTypeInfo;
}

SVGAnimatedString* SVGFEDisplacementMapElement::In1() {
  return AnimatedString(u"in");
}

SVGAnimatedString* SVGFEDisplacementMapElement::In2() {
  return AnimatedString(u"in2");
}

SVGAnimatedNumber* SVGFEDisplacementMapElement::Scale() {
  return AnimatedNumber(u"scale", 0);
}

SVGAnimatedEnumeration* SVGFEDisplacementMapElement::XChannelSelector() {
  return AnimatedEnumeration(u"xChannelSelector", kChannelSelectors, 4);
}

SVGAnimatedEnumeration* SVGFEDisplacementMapElement::YChannelSelector() {
  return AnimatedEnumeration(u"yChannelSelector", kChannelSelectors, 4);
}

// static
SVGFEDistantLightElement* SVGFEDistantLightElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGFEDistantLightElement>(document);
}

SVGFEDistantLightElement::SVGFEDistantLightElement(Document& document)
    : SVGElement(document, u"feDistantLight") {}

const bindings::WrapperTypeInfo* SVGFEDistantLightElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGFEDistantLightElement::kWrapperTypeInfo;
}

SVGAnimatedNumber* SVGFEDistantLightElement::Azimuth() {
  return AnimatedNumber(u"azimuth", 0);
}

SVGAnimatedNumber* SVGFEDistantLightElement::Elevation() {
  return AnimatedNumber(u"elevation", 0);
}

// static
SVGFEDropShadowElement* SVGFEDropShadowElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGFEDropShadowElement>(document);
}

SVGFEDropShadowElement::SVGFEDropShadowElement(Document& document)
    : SVGElement(document, u"feDropShadow") {}

const bindings::WrapperTypeInfo* SVGFEDropShadowElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGFEDropShadowElement::kWrapperTypeInfo;
}

SVGAnimatedString* SVGFEDropShadowElement::In1() {
  return AnimatedString(u"in");
}

SVGAnimatedNumber* SVGFEDropShadowElement::Dx() {
  return AnimatedNumber(u"dx", 2);
}

SVGAnimatedNumber* SVGFEDropShadowElement::Dy() {
  return AnimatedNumber(u"dy", 2);
}

SVGAnimatedNumber* SVGFEDropShadowElement::StdDeviationX() {
  return AnimatedNumberHalf(u"stdDeviation", SVGPairHalf::kFirst, 2);
}

SVGAnimatedNumber* SVGFEDropShadowElement::StdDeviationY() {
  return AnimatedNumberHalf(u"stdDeviation", SVGPairHalf::kSecond, 2);
}

void SVGFEDropShadowElement::SetStdDeviation(float std_deviation_x,
                                             float std_deviation_y) {
  StdDeviationX()->SetBaseValue(std_deviation_x);
  StdDeviationY()->SetBaseValue(std_deviation_y);
}

// static
SVGFEFloodElement* SVGFEFloodElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGFEFloodElement>(document);
}

SVGFEFloodElement::SVGFEFloodElement(Document& document)
    : SVGElement(document, u"feFlood") {}

const bindings::WrapperTypeInfo* SVGFEFloodElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGFEFloodElement::kWrapperTypeInfo;
}

// static
SVGFEFuncAElement* SVGFEFuncAElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGFEFuncAElement>(document);
}

SVGFEFuncAElement::SVGFEFuncAElement(Document& document)
    : SVGComponentTransferFunctionElement(document, u"feFuncA") {}

const bindings::WrapperTypeInfo* SVGFEFuncAElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGFEFuncAElement::kWrapperTypeInfo;
}

// static
SVGFEFuncBElement* SVGFEFuncBElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGFEFuncBElement>(document);
}

SVGFEFuncBElement::SVGFEFuncBElement(Document& document)
    : SVGComponentTransferFunctionElement(document, u"feFuncB") {}

const bindings::WrapperTypeInfo* SVGFEFuncBElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGFEFuncBElement::kWrapperTypeInfo;
}

// static
SVGFEFuncGElement* SVGFEFuncGElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGFEFuncGElement>(document);
}

SVGFEFuncGElement::SVGFEFuncGElement(Document& document)
    : SVGComponentTransferFunctionElement(document, u"feFuncG") {}

const bindings::WrapperTypeInfo* SVGFEFuncGElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGFEFuncGElement::kWrapperTypeInfo;
}

// static
SVGFEFuncRElement* SVGFEFuncRElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGFEFuncRElement>(document);
}

SVGFEFuncRElement::SVGFEFuncRElement(Document& document)
    : SVGComponentTransferFunctionElement(document, u"feFuncR") {}

const bindings::WrapperTypeInfo* SVGFEFuncRElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGFEFuncRElement::kWrapperTypeInfo;
}

// static
SVGFEGaussianBlurElement* SVGFEGaussianBlurElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGFEGaussianBlurElement>(document);
}

SVGFEGaussianBlurElement::SVGFEGaussianBlurElement(Document& document)
    : SVGElement(document, u"feGaussianBlur") {}

const bindings::WrapperTypeInfo* SVGFEGaussianBlurElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGFEGaussianBlurElement::kWrapperTypeInfo;
}

SVGAnimatedString* SVGFEGaussianBlurElement::In1() {
  return AnimatedString(u"in");
}

SVGAnimatedNumber* SVGFEGaussianBlurElement::StdDeviationX() {
  return AnimatedNumberHalf(u"stdDeviation", SVGPairHalf::kFirst, 0);
}

SVGAnimatedNumber* SVGFEGaussianBlurElement::StdDeviationY() {
  return AnimatedNumberHalf(u"stdDeviation", SVGPairHalf::kSecond, 0);
}

void SVGFEGaussianBlurElement::SetStdDeviation(float std_deviation_x,
                                               float std_deviation_y) {
  StdDeviationX()->SetBaseValue(std_deviation_x);
  StdDeviationY()->SetBaseValue(std_deviation_y);
}

// static
SVGFEImageElement* SVGFEImageElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGFEImageElement>(document);
}

SVGFEImageElement::SVGFEImageElement(Document& document)
    : SVGElement(document, u"feImage") {}

const bindings::WrapperTypeInfo* SVGFEImageElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGFEImageElement::kWrapperTypeInfo;
}

// static
SVGFEMergeElement* SVGFEMergeElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGFEMergeElement>(document);
}

SVGFEMergeElement::SVGFEMergeElement(Document& document)
    : SVGElement(document, u"feMerge") {}

const bindings::WrapperTypeInfo* SVGFEMergeElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGFEMergeElement::kWrapperTypeInfo;
}

// static
SVGFEMergeNodeElement* SVGFEMergeNodeElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGFEMergeNodeElement>(document);
}

SVGFEMergeNodeElement::SVGFEMergeNodeElement(Document& document)
    : SVGElement(document, u"feMergeNode") {}

const bindings::WrapperTypeInfo* SVGFEMergeNodeElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGFEMergeNodeElement::kWrapperTypeInfo;
}

SVGAnimatedString* SVGFEMergeNodeElement::In1() {
  return AnimatedString(u"in");
}

// static
SVGFEMorphologyElement* SVGFEMorphologyElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGFEMorphologyElement>(document);
}

SVGFEMorphologyElement::SVGFEMorphologyElement(Document& document)
    : SVGElement(document, u"feMorphology") {}

const bindings::WrapperTypeInfo* SVGFEMorphologyElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGFEMorphologyElement::kWrapperTypeInfo;
}

SVGAnimatedString* SVGFEMorphologyElement::In1() {
  return AnimatedString(u"in");
}

SVGAnimatedEnumeration* SVGFEMorphologyElement::SvgOperator() {
  return AnimatedEnumeration(u"operator", kMorphologyOperators, 1);
}

SVGAnimatedNumber* SVGFEMorphologyElement::RadiusX() {
  return AnimatedNumberHalf(u"radius", SVGPairHalf::kFirst, 0);
}

SVGAnimatedNumber* SVGFEMorphologyElement::RadiusY() {
  return AnimatedNumberHalf(u"radius", SVGPairHalf::kSecond, 0);
}

// static
SVGFEOffsetElement* SVGFEOffsetElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGFEOffsetElement>(document);
}

SVGFEOffsetElement::SVGFEOffsetElement(Document& document)
    : SVGElement(document, u"feOffset") {}

const bindings::WrapperTypeInfo* SVGFEOffsetElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGFEOffsetElement::kWrapperTypeInfo;
}

SVGAnimatedString* SVGFEOffsetElement::In1() {
  return AnimatedString(u"in");
}

SVGAnimatedNumber* SVGFEOffsetElement::Dx() {
  return AnimatedNumber(u"dx", 0);
}

SVGAnimatedNumber* SVGFEOffsetElement::Dy() {
  return AnimatedNumber(u"dy", 0);
}

// static
SVGFEPointLightElement* SVGFEPointLightElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGFEPointLightElement>(document);
}

SVGFEPointLightElement::SVGFEPointLightElement(Document& document)
    : SVGElement(document, u"fePointLight") {}

const bindings::WrapperTypeInfo* SVGFEPointLightElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGFEPointLightElement::kWrapperTypeInfo;
}

SVGAnimatedNumber* SVGFEPointLightElement::X() {
  return AnimatedNumber(u"x", 0);
}

SVGAnimatedNumber* SVGFEPointLightElement::Y() {
  return AnimatedNumber(u"y", 0);
}

SVGAnimatedNumber* SVGFEPointLightElement::Z() {
  return AnimatedNumber(u"z", 0);
}

// static
SVGFESpecularLightingElement* SVGFESpecularLightingElement::Create(
    Document& document) {
  return bindings::MakeGarbageCollected<SVGFESpecularLightingElement>(document);
}

SVGFESpecularLightingElement::SVGFESpecularLightingElement(Document& document)
    : SVGElement(document, u"feSpecularLighting") {}

const bindings::WrapperTypeInfo*
SVGFESpecularLightingElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGFESpecularLightingElement::kWrapperTypeInfo;
}

SVGAnimatedString* SVGFESpecularLightingElement::In1() {
  return AnimatedString(u"in");
}

SVGAnimatedNumber* SVGFESpecularLightingElement::SurfaceScale() {
  return AnimatedNumber(u"surfaceScale", 1);
}

SVGAnimatedNumber* SVGFESpecularLightingElement::SpecularConstant() {
  return AnimatedNumber(u"specularConstant", 1);
}

SVGAnimatedNumber* SVGFESpecularLightingElement::SpecularExponent() {
  return AnimatedNumber(u"specularExponent", 1);
}

SVGAnimatedNumber* SVGFESpecularLightingElement::KernelUnitLengthX() {
  return AnimatedNumberHalf(u"kernelUnitLength", SVGPairHalf::kFirst, 0);
}

SVGAnimatedNumber* SVGFESpecularLightingElement::KernelUnitLengthY() {
  return AnimatedNumberHalf(u"kernelUnitLength", SVGPairHalf::kSecond, 0);
}

// static
SVGFESpotLightElement* SVGFESpotLightElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGFESpotLightElement>(document);
}

SVGFESpotLightElement::SVGFESpotLightElement(Document& document)
    : SVGElement(document, u"feSpotLight") {}

const bindings::WrapperTypeInfo* SVGFESpotLightElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGFESpotLightElement::kWrapperTypeInfo;
}

SVGAnimatedNumber* SVGFESpotLightElement::X() {
  return AnimatedNumber(u"x", 0);
}

SVGAnimatedNumber* SVGFESpotLightElement::Y() {
  return AnimatedNumber(u"y", 0);
}

SVGAnimatedNumber* SVGFESpotLightElement::Z() {
  return AnimatedNumber(u"z", 0);
}

SVGAnimatedNumber* SVGFESpotLightElement::PointsAtX() {
  return AnimatedNumber(u"pointsAtX", 0);
}

SVGAnimatedNumber* SVGFESpotLightElement::PointsAtY() {
  return AnimatedNumber(u"pointsAtY", 0);
}

SVGAnimatedNumber* SVGFESpotLightElement::PointsAtZ() {
  return AnimatedNumber(u"pointsAtZ", 0);
}

SVGAnimatedNumber* SVGFESpotLightElement::SpecularExponent() {
  return AnimatedNumber(u"specularExponent", 1);
}

SVGAnimatedNumber* SVGFESpotLightElement::LimitingConeAngle() {
  return AnimatedNumber(u"limitingConeAngle", 0);
}

// static
SVGFETileElement* SVGFETileElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGFETileElement>(document);
}

SVGFETileElement::SVGFETileElement(Document& document)
    : SVGElement(document, u"feTile") {}

const bindings::WrapperTypeInfo* SVGFETileElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGFETileElement::kWrapperTypeInfo;
}

SVGAnimatedString* SVGFETileElement::In1() {
  return AnimatedString(u"in");
}

// static
SVGFETurbulenceElement* SVGFETurbulenceElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGFETurbulenceElement>(document);
}

SVGFETurbulenceElement::SVGFETurbulenceElement(Document& document)
    : SVGElement(document, u"feTurbulence") {}

const bindings::WrapperTypeInfo* SVGFETurbulenceElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGFETurbulenceElement::kWrapperTypeInfo;
}

SVGAnimatedNumber* SVGFETurbulenceElement::BaseFrequencyX() {
  return AnimatedNumberHalf(u"baseFrequency", SVGPairHalf::kFirst, 0);
}

SVGAnimatedNumber* SVGFETurbulenceElement::BaseFrequencyY() {
  return AnimatedNumberHalf(u"baseFrequency", SVGPairHalf::kSecond, 0);
}

SVGAnimatedInteger* SVGFETurbulenceElement::NumOctaves() {
  return AnimatedInteger(u"numOctaves", 1);
}

SVGAnimatedNumber* SVGFETurbulenceElement::Seed() {
  return AnimatedNumber(u"seed", 0);
}

SVGAnimatedEnumeration* SVGFETurbulenceElement::StitchTiles() {
  return AnimatedEnumeration(u"stitchTiles", kStitchTypes, 2);
}

SVGAnimatedEnumeration* SVGFETurbulenceElement::Type() {
  return AnimatedEnumeration(u"type", kTurbulenceTypes, 2);
}

// static
SVGFilterElement* SVGFilterElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGFilterElement>(document);
}

SVGFilterElement::SVGFilterElement(Document& document)
    : SVGElement(document, u"filter") {}

const bindings::WrapperTypeInfo* SVGFilterElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGFilterElement::kWrapperTypeInfo;
}

SVGAnimatedEnumeration* SVGFilterElement::FilterUnits() {
  return AnimatedEnumeration(u"filterUnits", kUnitTypes, 2);
}

SVGAnimatedEnumeration* SVGFilterElement::PrimitiveUnits() {
  return AnimatedEnumeration(u"primitiveUnits", kUnitTypes, 1);
}

// static
SVGForeignObjectElement* SVGForeignObjectElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGForeignObjectElement>(document);
}

SVGForeignObjectElement::SVGForeignObjectElement(Document& document)
    : SVGGraphicsElement(document, u"foreignObject") {}

const bindings::WrapperTypeInfo* SVGForeignObjectElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGForeignObjectElement::kWrapperTypeInfo;
}

// static
SVGGElement* SVGGElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGGElement>(document);
}

SVGGElement::SVGGElement(Document& document)
    : SVGGraphicsElement(document, u"g") {}

const bindings::WrapperTypeInfo* SVGGElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGGElement::kWrapperTypeInfo;
}

// static
SVGImageElement* SVGImageElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGImageElement>(document);
}

SVGImageElement::SVGImageElement(Document& document)
    : SVGGraphicsElement(document, u"image") {}

const bindings::WrapperTypeInfo* SVGImageElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGImageElement::kWrapperTypeInfo;
}

// static
SVGLineElement* SVGLineElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGLineElement>(document);
}

SVGLineElement::SVGLineElement(Document& document)
    : SVGGeometryElement(document, u"line") {}

const bindings::WrapperTypeInfo* SVGLineElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGLineElement::kWrapperTypeInfo;
}

// static
SVGLinearGradientElement* SVGLinearGradientElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGLinearGradientElement>(document);
}

SVGLinearGradientElement::SVGLinearGradientElement(Document& document)
    : SVGGradientElement(document, u"linearGradient") {}

const bindings::WrapperTypeInfo* SVGLinearGradientElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGLinearGradientElement::kWrapperTypeInfo;
}

// static
SVGMPathElement* SVGMPathElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGMPathElement>(document);
}

SVGMPathElement::SVGMPathElement(Document& document)
    : SVGElement(document, u"mpath") {}

const bindings::WrapperTypeInfo* SVGMPathElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGMPathElement::kWrapperTypeInfo;
}

// static
SVGMarkerElement* SVGMarkerElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGMarkerElement>(document);
}

SVGMarkerElement::SVGMarkerElement(Document& document)
    : SVGElement(document, u"marker") {}

const bindings::WrapperTypeInfo* SVGMarkerElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGMarkerElement::kWrapperTypeInfo;
}

SVGAnimatedEnumeration* SVGMarkerElement::MarkerUnits() {
  return AnimatedEnumeration(u"markerUnits", kMarkerUnits, 2);
}

// static
SVGMaskElement* SVGMaskElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGMaskElement>(document);
}

SVGMaskElement::SVGMaskElement(Document& document)
    : SVGElement(document, u"mask") {}

const bindings::WrapperTypeInfo* SVGMaskElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGMaskElement::kWrapperTypeInfo;
}

SVGAnimatedEnumeration* SVGMaskElement::MaskUnits() {
  return AnimatedEnumeration(u"maskUnits", kUnitTypes, 2);
}

SVGAnimatedEnumeration* SVGMaskElement::MaskContentUnits() {
  return AnimatedEnumeration(u"maskContentUnits", kUnitTypes, 1);
}

// static
SVGMetadataElement* SVGMetadataElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGMetadataElement>(document);
}

SVGMetadataElement::SVGMetadataElement(Document& document)
    : SVGElement(document, u"metadata") {}

const bindings::WrapperTypeInfo* SVGMetadataElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGMetadataElement::kWrapperTypeInfo;
}

// static
SVGPathElement* SVGPathElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGPathElement>(document);
}

SVGPathElement::SVGPathElement(Document& document)
    : SVGGeometryElement(document, u"path") {}

const bindings::WrapperTypeInfo* SVGPathElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGPathElement::kWrapperTypeInfo;
}

// static
SVGPatternElement* SVGPatternElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGPatternElement>(document);
}

SVGPatternElement::SVGPatternElement(Document& document)
    : SVGElement(document, u"pattern") {}

const bindings::WrapperTypeInfo* SVGPatternElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGPatternElement::kWrapperTypeInfo;
}

SVGAnimatedEnumeration* SVGPatternElement::PatternUnits() {
  return AnimatedEnumeration(u"patternUnits", kUnitTypes, 2);
}

SVGAnimatedEnumeration* SVGPatternElement::PatternContentUnits() {
  return AnimatedEnumeration(u"patternContentUnits", kUnitTypes, 1);
}

// static
SVGPolygonElement* SVGPolygonElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGPolygonElement>(document);
}

SVGPolygonElement::SVGPolygonElement(Document& document)
    : SVGGeometryElement(document, u"polygon") {}

const bindings::WrapperTypeInfo* SVGPolygonElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGPolygonElement::kWrapperTypeInfo;
}

// static
SVGPolylineElement* SVGPolylineElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGPolylineElement>(document);
}

SVGPolylineElement::SVGPolylineElement(Document& document)
    : SVGGeometryElement(document, u"polyline") {}

const bindings::WrapperTypeInfo* SVGPolylineElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGPolylineElement::kWrapperTypeInfo;
}

// static
SVGRadialGradientElement* SVGRadialGradientElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGRadialGradientElement>(document);
}

SVGRadialGradientElement::SVGRadialGradientElement(Document& document)
    : SVGGradientElement(document, u"radialGradient") {}

const bindings::WrapperTypeInfo* SVGRadialGradientElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGRadialGradientElement::kWrapperTypeInfo;
}

// static
SVGRectElement* SVGRectElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGRectElement>(document);
}

SVGRectElement::SVGRectElement(Document& document)
    : SVGGeometryElement(document, u"rect") {}

const bindings::WrapperTypeInfo* SVGRectElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGRectElement::kWrapperTypeInfo;
}

// static
SVGSVGElement* SVGSVGElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGSVGElement>(document);
}

SVGSVGElement::SVGSVGElement(Document& document)
    : SVGGraphicsElement(document, u"svg") {}

const bindings::WrapperTypeInfo* SVGSVGElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGSVGElement::kWrapperTypeInfo;
}

// static
SVGScriptElement* SVGScriptElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGScriptElement>(document);
}

SVGScriptElement::SVGScriptElement(Document& document)
    : SVGElement(document, u"script") {}

const bindings::WrapperTypeInfo* SVGScriptElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGScriptElement::kWrapperTypeInfo;
}

// static
SVGSetElement* SVGSetElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGSetElement>(document);
}

SVGSetElement::SVGSetElement(Document& document)
    : SVGAnimationElement(document, u"set") {}

const bindings::WrapperTypeInfo* SVGSetElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGSetElement::kWrapperTypeInfo;
}

// static
SVGStopElement* SVGStopElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGStopElement>(document);
}

SVGStopElement::SVGStopElement(Document& document)
    : SVGElement(document, u"stop") {}

const bindings::WrapperTypeInfo* SVGStopElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGStopElement::kWrapperTypeInfo;
}

SVGAnimatedNumber* SVGStopElement::Offset() {
  return AnimatedNumber(u"offset", 0,
                        SVGAnimatedNumber::Syntax::kNumberOrPercentage);
}

// static
SVGStyleElement* SVGStyleElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGStyleElement>(document);
}

SVGStyleElement::SVGStyleElement(Document& document)
    : SVGElement(document, u"style") {}

const bindings::WrapperTypeInfo* SVGStyleElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGStyleElement::kWrapperTypeInfo;
}

// static
SVGSwitchElement* SVGSwitchElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGSwitchElement>(document);
}

SVGSwitchElement::SVGSwitchElement(Document& document)
    : SVGGraphicsElement(document, u"switch") {}

const bindings::WrapperTypeInfo* SVGSwitchElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGSwitchElement::kWrapperTypeInfo;
}

// static
SVGSymbolElement* SVGSymbolElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGSymbolElement>(document);
}

SVGSymbolElement::SVGSymbolElement(Document& document)
    : SVGGraphicsElement(document, u"symbol") {}

const bindings::WrapperTypeInfo* SVGSymbolElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGSymbolElement::kWrapperTypeInfo;
}

// static
SVGTSpanElement* SVGTSpanElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGTSpanElement>(document);
}

SVGTSpanElement::SVGTSpanElement(Document& document)
    : SVGTextPositioningElement(document, u"tspan") {}

const bindings::WrapperTypeInfo* SVGTSpanElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGTSpanElement::kWrapperTypeInfo;
}

// static
SVGTextElement* SVGTextElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGTextElement>(document);
}

SVGTextElement::SVGTextElement(Document& document)
    : SVGTextPositioningElement(document, u"text") {}

const bindings::WrapperTypeInfo* SVGTextElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGTextElement::kWrapperTypeInfo;
}

// static
SVGTextPathElement* SVGTextPathElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGTextPathElement>(document);
}

SVGTextPathElement::SVGTextPathElement(Document& document)
    : SVGTextContentElement(document, u"textPath") {}

const bindings::WrapperTypeInfo* SVGTextPathElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGTextPathElement::kWrapperTypeInfo;
}

SVGAnimatedEnumeration* SVGTextPathElement::Method() {
  return AnimatedEnumeration(u"method", kTextPathMethods, 1);
}

SVGAnimatedEnumeration* SVGTextPathElement::Spacing() {
  return AnimatedEnumeration(u"spacing", kTextPathSpacings, 2);
}

SVGAnimatedEnumeration* SVGTextPathElement::Side() {
  return AnimatedEnumeration(u"side", kTextPathSides, 1);
}

// static
SVGTitleElement* SVGTitleElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGTitleElement>(document);
}

SVGTitleElement::SVGTitleElement(Document& document)
    : SVGElement(document, u"title") {}

const bindings::WrapperTypeInfo* SVGTitleElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGTitleElement::kWrapperTypeInfo;
}

// static
SVGUseElement* SVGUseElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGUseElement>(document);
}

SVGUseElement::SVGUseElement(Document& document)
    : SVGGraphicsElement(document, u"use") {}

const bindings::WrapperTypeInfo* SVGUseElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGUseElement::kWrapperTypeInfo;
}

// static
SVGViewElement* SVGViewElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGViewElement>(document);
}

SVGViewElement::SVGViewElement(Document& document)
    : SVGElement(document, u"view") {}

const bindings::WrapperTypeInfo* SVGViewElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGViewElement::kWrapperTypeInfo;
}

}  // namespace mitto
