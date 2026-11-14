// The elements of SVG, whose classes have their interfaces' animated values,
// lists, rectangles, aspect ratios, angles and points: each other member of
// theirs that is not a reflected attribute is a Known Gap. Blink's classes of
// the same names hold what layout and animation need.
//
// Upstream: third_party/blink/renderer/core/svg/svg_tag_names.json5

#ifndef MITTO_DOM_SVG_ELEMENTS_H_
#define MITTO_DOM_SVG_ELEMENTS_H_

#include <stdint.h>

#include <string>

#include "mitto/dom/svg_element.h"

namespace mitto {

class SVGAnimationElement : public SVGElement {
 public:
  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

 protected:
  SVGAnimationElement(Document& document, const std::u16string& local_name);
};

class SVGComponentTransferFunctionElement : public SVGElement {
 public:
  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedNumberList* TableValues();
  SVGAnimatedEnumeration* Type();
  SVGAnimatedNumber* Slope();
  SVGAnimatedNumber* Intercept();
  SVGAnimatedNumber* Amplitude();
  SVGAnimatedNumber* Exponent();
  SVGAnimatedNumber* Offset();

 protected:
  SVGComponentTransferFunctionElement(Document& document,
                                      const std::u16string& local_name);
};

// SVGFilterPrimitiveStandardAttributes, which the filter primitives include:
// their subregion's lengths, as Blink's class of that name has them.
class SVGFilterPrimitiveStandardAttributes : public SVGElement {
 public:
  SVGAnimatedLength* X();
  SVGAnimatedLength* Y();
  SVGAnimatedLength* Width();
  SVGAnimatedLength* Height();

 protected:
  SVGFilterPrimitiveStandardAttributes(Document& document,
                                       const std::u16string& local_name);
};

class SVGGradientElement : public SVGElement {
 public:
  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedEnumeration* GradientUnits();
  SVGAnimatedEnumeration* SpreadMethod();

 protected:
  SVGGradientElement(Document& document, const std::u16string& local_name);
};

class SVGGraphicsElement : public SVGElement {
 public:
  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

 protected:
  SVGGraphicsElement(Document& document, const std::u16string& local_name);
};

class SVGGeometryElement : public SVGGraphicsElement {
 public:
  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedNumber* PathLength();

 protected:
  SVGGeometryElement(Document& document, const std::u16string& local_name);
};

class SVGTextContentElement : public SVGGraphicsElement {
 public:
  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedLength* TextLength();
  SVGAnimatedEnumeration* LengthAdjust();

 protected:
  SVGTextContentElement(Document& document, const std::u16string& local_name);
};

class SVGTextPositioningElement : public SVGTextContentElement {
 public:
  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedLengthList* X();
  SVGAnimatedLengthList* Y();
  SVGAnimatedLengthList* Dx();
  SVGAnimatedLengthList* Dy();
  SVGAnimatedNumberList* Rotate();

 protected:
  SVGTextPositioningElement(Document& document,
                            const std::u16string& local_name);
};

class SVGAElement final : public SVGGraphicsElement {
 public:
  static SVGAElement* Create(Document& document);

  explicit SVGAElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedString* SvgTarget();
  int32_t DefaultTabIndex() const override;
  // Node's: activation behaviour, href or not, as Blink's SVGAElement has.
  bool HasActivationBehavior() const override;
  DOMTokenList* RelList() {
    return EnsureTokenList(u"rel", DOMTokenList::SupportedTokens::kRel);
  }
};

class SVGAnimateElement final : public SVGAnimationElement {
 public:
  static SVGAnimateElement* Create(Document& document);

  explicit SVGAnimateElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class SVGAnimateMotionElement final : public SVGAnimationElement {
 public:
  static SVGAnimateMotionElement* Create(Document& document);

  explicit SVGAnimateMotionElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class SVGAnimateTransformElement final : public SVGAnimationElement {
 public:
  static SVGAnimateTransformElement* Create(Document& document);

  explicit SVGAnimateTransformElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class SVGCircleElement final : public SVGGeometryElement {
 public:
  static SVGCircleElement* Create(Document& document);

  explicit SVGCircleElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedLength* Cx();
  SVGAnimatedLength* Cy();
  SVGAnimatedLength* R();
};

class SVGClipPathElement final : public SVGElement {
 public:
  static SVGClipPathElement* Create(Document& document);

  explicit SVGClipPathElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedEnumeration* ClipPathUnits();
};

class SVGDefsElement final : public SVGGraphicsElement {
 public:
  static SVGDefsElement* Create(Document& document);

  explicit SVGDefsElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class SVGDescElement final : public SVGElement {
 public:
  static SVGDescElement* Create(Document& document);

  explicit SVGDescElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class SVGEllipseElement final : public SVGGeometryElement {
 public:
  static SVGEllipseElement* Create(Document& document);

  explicit SVGEllipseElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedLength* Cx();
  SVGAnimatedLength* Cy();
  SVGAnimatedLength* Rx();
  SVGAnimatedLength* Ry();
};

class SVGFEBlendElement final : public SVGFilterPrimitiveStandardAttributes {
 public:
  static SVGFEBlendElement* Create(Document& document);

  explicit SVGFEBlendElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedString* In1();
  SVGAnimatedString* In2();
  SVGAnimatedEnumeration* Mode();
};

class SVGFEColorMatrixElement final
    : public SVGFilterPrimitiveStandardAttributes {
 public:
  static SVGFEColorMatrixElement* Create(Document& document);

  explicit SVGFEColorMatrixElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedNumberList* Values();
  SVGAnimatedString* In1();
  SVGAnimatedEnumeration* Type();
};

class SVGFEComponentTransferElement final
    : public SVGFilterPrimitiveStandardAttributes {
 public:
  static SVGFEComponentTransferElement* Create(Document& document);

  explicit SVGFEComponentTransferElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedString* In1();
};

class SVGFECompositeElement final
    : public SVGFilterPrimitiveStandardAttributes {
 public:
  static SVGFECompositeElement* Create(Document& document);

  explicit SVGFECompositeElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedString* In2();
  SVGAnimatedString* In1();
  SVGAnimatedEnumeration* SvgOperator();
  SVGAnimatedNumber* K1();
  SVGAnimatedNumber* K2();
  SVGAnimatedNumber* K3();
  SVGAnimatedNumber* K4();
};

class SVGFEConvolveMatrixElement final
    : public SVGFilterPrimitiveStandardAttributes {
 public:
  static SVGFEConvolveMatrixElement* Create(Document& document);

  explicit SVGFEConvolveMatrixElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedNumberList* KernelMatrix();
  SVGAnimatedString* In1();
  SVGAnimatedInteger* OrderX();
  SVGAnimatedInteger* OrderY();
  SVGAnimatedNumber* Divisor();
  SVGAnimatedNumber* Bias();
  SVGAnimatedInteger* TargetX();
  SVGAnimatedInteger* TargetY();
  SVGAnimatedEnumeration* EdgeMode();
  SVGAnimatedNumber* KernelUnitLengthX();
  SVGAnimatedNumber* KernelUnitLengthY();
  SVGAnimatedBoolean* PreserveAlpha();
};

class SVGFEDiffuseLightingElement final
    : public SVGFilterPrimitiveStandardAttributes {
 public:
  static SVGFEDiffuseLightingElement* Create(Document& document);

  explicit SVGFEDiffuseLightingElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedString* In1();
  SVGAnimatedNumber* SurfaceScale();
  SVGAnimatedNumber* DiffuseConstant();
  SVGAnimatedNumber* KernelUnitLengthX();
  SVGAnimatedNumber* KernelUnitLengthY();
};

class SVGFEDisplacementMapElement final
    : public SVGFilterPrimitiveStandardAttributes {
 public:
  static SVGFEDisplacementMapElement* Create(Document& document);

  explicit SVGFEDisplacementMapElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedString* In1();
  SVGAnimatedString* In2();
  SVGAnimatedNumber* Scale();
  SVGAnimatedEnumeration* XChannelSelector();
  SVGAnimatedEnumeration* YChannelSelector();
};

class SVGFEDistantLightElement final : public SVGElement {
 public:
  static SVGFEDistantLightElement* Create(Document& document);

  explicit SVGFEDistantLightElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedNumber* Azimuth();
  SVGAnimatedNumber* Elevation();
};

class SVGFEDropShadowElement final
    : public SVGFilterPrimitiveStandardAttributes {
 public:
  static SVGFEDropShadowElement* Create(Document& document);

  explicit SVGFEDropShadowElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedString* In1();
  SVGAnimatedNumber* Dx();
  SVGAnimatedNumber* Dy();
  SVGAnimatedNumber* StdDeviationX();
  SVGAnimatedNumber* StdDeviationY();
  void SetStdDeviation(float std_deviation_x, float std_deviation_y);
};

class SVGFEFloodElement final : public SVGFilterPrimitiveStandardAttributes {
 public:
  static SVGFEFloodElement* Create(Document& document);

  explicit SVGFEFloodElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class SVGFEFuncAElement final : public SVGComponentTransferFunctionElement {
 public:
  static SVGFEFuncAElement* Create(Document& document);

  explicit SVGFEFuncAElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class SVGFEFuncBElement final : public SVGComponentTransferFunctionElement {
 public:
  static SVGFEFuncBElement* Create(Document& document);

  explicit SVGFEFuncBElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class SVGFEFuncGElement final : public SVGComponentTransferFunctionElement {
 public:
  static SVGFEFuncGElement* Create(Document& document);

  explicit SVGFEFuncGElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class SVGFEFuncRElement final : public SVGComponentTransferFunctionElement {
 public:
  static SVGFEFuncRElement* Create(Document& document);

  explicit SVGFEFuncRElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class SVGFEGaussianBlurElement final
    : public SVGFilterPrimitiveStandardAttributes {
 public:
  static SVGFEGaussianBlurElement* Create(Document& document);

  explicit SVGFEGaussianBlurElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedString* In1();
  SVGAnimatedNumber* StdDeviationX();
  SVGAnimatedNumber* StdDeviationY();
  void SetStdDeviation(float std_deviation_x, float std_deviation_y);
};

class SVGFEImageElement final : public SVGFilterPrimitiveStandardAttributes {
 public:
  static SVGFEImageElement* Create(Document& document);

  explicit SVGFEImageElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class SVGFEMergeElement final : public SVGFilterPrimitiveStandardAttributes {
 public:
  static SVGFEMergeElement* Create(Document& document);

  explicit SVGFEMergeElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class SVGFEMergeNodeElement final : public SVGElement {
 public:
  static SVGFEMergeNodeElement* Create(Document& document);

  explicit SVGFEMergeNodeElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedString* In1();
};

class SVGFEMorphologyElement final
    : public SVGFilterPrimitiveStandardAttributes {
 public:
  static SVGFEMorphologyElement* Create(Document& document);

  explicit SVGFEMorphologyElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedString* In1();
  SVGAnimatedEnumeration* SvgOperator();
  SVGAnimatedNumber* RadiusX();
  SVGAnimatedNumber* RadiusY();
};

class SVGFEOffsetElement final : public SVGFilterPrimitiveStandardAttributes {
 public:
  static SVGFEOffsetElement* Create(Document& document);

  explicit SVGFEOffsetElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedString* In1();
  SVGAnimatedNumber* Dx();
  SVGAnimatedNumber* Dy();
};

class SVGFEPointLightElement final : public SVGElement {
 public:
  static SVGFEPointLightElement* Create(Document& document);

  explicit SVGFEPointLightElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedNumber* X();
  SVGAnimatedNumber* Y();
  SVGAnimatedNumber* Z();
};

class SVGFESpecularLightingElement final
    : public SVGFilterPrimitiveStandardAttributes {
 public:
  static SVGFESpecularLightingElement* Create(Document& document);

  explicit SVGFESpecularLightingElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedString* In1();
  SVGAnimatedNumber* SurfaceScale();
  SVGAnimatedNumber* SpecularConstant();
  SVGAnimatedNumber* SpecularExponent();
  SVGAnimatedNumber* KernelUnitLengthX();
  SVGAnimatedNumber* KernelUnitLengthY();
};

class SVGFESpotLightElement final : public SVGElement {
 public:
  static SVGFESpotLightElement* Create(Document& document);

  explicit SVGFESpotLightElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedNumber* X();
  SVGAnimatedNumber* Y();
  SVGAnimatedNumber* Z();
  SVGAnimatedNumber* PointsAtX();
  SVGAnimatedNumber* PointsAtY();
  SVGAnimatedNumber* PointsAtZ();
  SVGAnimatedNumber* SpecularExponent();
  SVGAnimatedNumber* LimitingConeAngle();
};

class SVGFETileElement final : public SVGFilterPrimitiveStandardAttributes {
 public:
  static SVGFETileElement* Create(Document& document);

  explicit SVGFETileElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedString* In1();
};

class SVGFETurbulenceElement final
    : public SVGFilterPrimitiveStandardAttributes {
 public:
  static SVGFETurbulenceElement* Create(Document& document);

  explicit SVGFETurbulenceElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedNumber* BaseFrequencyX();
  SVGAnimatedNumber* BaseFrequencyY();
  SVGAnimatedInteger* NumOctaves();
  SVGAnimatedNumber* Seed();
  SVGAnimatedEnumeration* StitchTiles();
  SVGAnimatedEnumeration* Type();
};

class SVGFilterElement final : public SVGElement {
 public:
  static SVGFilterElement* Create(Document& document);

  explicit SVGFilterElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedLength* X();
  SVGAnimatedLength* Y();
  SVGAnimatedLength* Width();
  SVGAnimatedLength* Height();
  SVGAnimatedEnumeration* FilterUnits();
  SVGAnimatedEnumeration* PrimitiveUnits();
};

class SVGForeignObjectElement final : public SVGGraphicsElement {
 public:
  static SVGForeignObjectElement* Create(Document& document);

  explicit SVGForeignObjectElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedLength* X();
  SVGAnimatedLength* Y();
  SVGAnimatedLength* Width();
  SVGAnimatedLength* Height();
};

class SVGGElement final : public SVGGraphicsElement {
 public:
  static SVGGElement* Create(Document& document);

  explicit SVGGElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class SVGImageElement final : public SVGGraphicsElement {
 public:
  static SVGImageElement* Create(Document& document);

  explicit SVGImageElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedLength* X();
  SVGAnimatedLength* Y();
  SVGAnimatedLength* Width();
  SVGAnimatedLength* Height();
};

class SVGLineElement final : public SVGGeometryElement {
 public:
  static SVGLineElement* Create(Document& document);

  explicit SVGLineElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedLength* X1();
  SVGAnimatedLength* Y1();
  SVGAnimatedLength* X2();
  SVGAnimatedLength* Y2();
};

class SVGLinearGradientElement final : public SVGGradientElement {
 public:
  static SVGLinearGradientElement* Create(Document& document);

  explicit SVGLinearGradientElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedLength* X1();
  SVGAnimatedLength* Y1();
  SVGAnimatedLength* X2();
  SVGAnimatedLength* Y2();
};

class SVGMPathElement final : public SVGElement {
 public:
  static SVGMPathElement* Create(Document& document);

  explicit SVGMPathElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class SVGMarkerElement final : public SVGElement {
 public:
  static SVGMarkerElement* Create(Document& document);

  explicit SVGMarkerElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedLength* RefX();
  SVGAnimatedLength* RefY();
  SVGAnimatedLength* MarkerWidth();
  SVGAnimatedLength* MarkerHeight();
  SVGAnimatedEnumeration* MarkerUnits();
  SVGAnimatedEnumeration* OrientType();
  SVGAnimatedAngle* OrientAngle();
  // Each sets the attribute, as a page would, with a record for mutation
  // observers.
  void SetOrientToAuto();
  void SetOrientToAngle(SVGAngle* angle);
};

class SVGMaskElement final : public SVGElement {
 public:
  static SVGMaskElement* Create(Document& document);

  explicit SVGMaskElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedLength* X();
  SVGAnimatedLength* Y();
  SVGAnimatedLength* Width();
  SVGAnimatedLength* Height();
  SVGAnimatedEnumeration* MaskUnits();
  SVGAnimatedEnumeration* MaskContentUnits();
};

class SVGMetadataElement final : public SVGElement {
 public:
  static SVGMetadataElement* Create(Document& document);

  explicit SVGMetadataElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class SVGPathElement final : public SVGGeometryElement {
 public:
  static SVGPathElement* Create(Document& document);

  explicit SVGPathElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class SVGPatternElement final : public SVGElement {
 public:
  static SVGPatternElement* Create(Document& document);

  explicit SVGPatternElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedLength* X();
  SVGAnimatedLength* Y();
  SVGAnimatedLength* Width();
  SVGAnimatedLength* Height();
  SVGAnimatedEnumeration* PatternUnits();
  SVGAnimatedEnumeration* PatternContentUnits();
};

class SVGPolygonElement final : public SVGGeometryElement {
 public:
  static SVGPolygonElement* Create(Document& document);

  explicit SVGPolygonElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGPointList* PointsFromJavascript();
  SVGPointList* AnimatedPoints();
};

class SVGPolylineElement final : public SVGGeometryElement {
 public:
  static SVGPolylineElement* Create(Document& document);

  explicit SVGPolylineElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGPointList* PointsFromJavascript();
  SVGPointList* AnimatedPoints();
};

class SVGRadialGradientElement final : public SVGGradientElement {
 public:
  static SVGRadialGradientElement* Create(Document& document);

  explicit SVGRadialGradientElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedLength* Cx();
  SVGAnimatedLength* Cy();
  SVGAnimatedLength* R();
  SVGAnimatedLength* Fx();
  SVGAnimatedLength* Fy();
  SVGAnimatedLength* Fr();
};

class SVGRectElement final : public SVGGeometryElement {
 public:
  static SVGRectElement* Create(Document& document);

  explicit SVGRectElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedLength* X();
  SVGAnimatedLength* Y();
  SVGAnimatedLength* Width();
  SVGAnimatedLength* Height();
  SVGAnimatedLength* Rx();
  SVGAnimatedLength* Ry();
};

class SVGSVGElement final : public SVGGraphicsElement {
 public:
  static SVGSVGElement* Create(Document& document);

  explicit SVGSVGElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedLength* X();
  SVGAnimatedLength* Y();
  SVGAnimatedLength* Width();
  SVGAnimatedLength* Height();
  SVGAngle* CreateSVGAngle();
  SVGLength* CreateSVGLength();
  SVGNumber* CreateSVGNumber();
  SVGPoint* CreateSVGPoint();
  SVGRect* CreateSVGRect();
};

class SVGScriptElement final : public SVGElement {
 public:
  static SVGScriptElement* Create(Document& document);

  explicit SVGScriptElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class SVGSetElement final : public SVGAnimationElement {
 public:
  static SVGSetElement* Create(Document& document);

  explicit SVGSetElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class SVGStopElement final : public SVGElement {
 public:
  static SVGStopElement* Create(Document& document);

  explicit SVGStopElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedNumber* Offset();
};

class SVGStyleElement final : public SVGElement {
 public:
  static SVGStyleElement* Create(Document& document);

  explicit SVGStyleElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class SVGSwitchElement final : public SVGGraphicsElement {
 public:
  static SVGSwitchElement* Create(Document& document);

  explicit SVGSwitchElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class SVGSymbolElement final : public SVGGraphicsElement {
 public:
  static SVGSymbolElement* Create(Document& document);

  explicit SVGSymbolElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class SVGTSpanElement final : public SVGTextPositioningElement {
 public:
  static SVGTSpanElement* Create(Document& document);

  explicit SVGTSpanElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class SVGTextElement final : public SVGTextPositioningElement {
 public:
  static SVGTextElement* Create(Document& document);

  explicit SVGTextElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class SVGTextPathElement final : public SVGTextContentElement {
 public:
  static SVGTextPathElement* Create(Document& document);

  explicit SVGTextPathElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedLength* StartOffset();
  SVGAnimatedEnumeration* Method();
  SVGAnimatedEnumeration* Spacing();
  SVGAnimatedEnumeration* Side();
};

class SVGTitleElement final : public SVGElement {
 public:
  static SVGTitleElement* Create(Document& document);

  explicit SVGTitleElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class SVGUseElement final : public SVGGraphicsElement {
 public:
  static SVGUseElement* Create(Document& document);

  explicit SVGUseElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  SVGAnimatedLength* X();
  SVGAnimatedLength* Y();
  SVGAnimatedLength* Width();
  SVGAnimatedLength* Height();
};

class SVGViewElement final : public SVGElement {
 public:
  static SVGViewElement* Create(Document& document);

  explicit SVGViewElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_SVG_ELEMENTS_H_
