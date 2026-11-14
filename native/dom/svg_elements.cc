#include "mitto/dom/svg_elements.h"

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

SVGGradientElement::SVGGradientElement(Document& document,
                                       const std::u16string& local_name)
    : SVGElement(document, local_name) {}

const bindings::WrapperTypeInfo* SVGGradientElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGGradientElement::kWrapperTypeInfo;
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

SVGTextContentElement::SVGTextContentElement(Document& document,
                                             const std::u16string& local_name)
    : SVGGraphicsElement(document, local_name) {}

const bindings::WrapperTypeInfo* SVGTextContentElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGTextContentElement::kWrapperTypeInfo;
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

// static
SVGFETileElement* SVGFETileElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGFETileElement>(document);
}

SVGFETileElement::SVGFETileElement(Document& document)
    : SVGElement(document, u"feTile") {}

const bindings::WrapperTypeInfo* SVGFETileElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGFETileElement::kWrapperTypeInfo;
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

// static
SVGFilterElement* SVGFilterElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGFilterElement>(document);
}

SVGFilterElement::SVGFilterElement(Document& document)
    : SVGElement(document, u"filter") {}

const bindings::WrapperTypeInfo* SVGFilterElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGFilterElement::kWrapperTypeInfo;
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

// static
SVGMaskElement* SVGMaskElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGMaskElement>(document);
}

SVGMaskElement::SVGMaskElement(Document& document)
    : SVGElement(document, u"mask") {}

const bindings::WrapperTypeInfo* SVGMaskElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGMaskElement::kWrapperTypeInfo;
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
