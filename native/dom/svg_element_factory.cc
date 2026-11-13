#include "mitto/dom/svg_element_factory.h"

#include <algorithm>
#include <array>
#include <string_view>

#include "mitto/dom/svg_element.h"
#include "mitto/dom/svg_elements.h"

// Upstream: third_party/blink/renderer/core/svg/svg_tag_names.json5

namespace mitto {

namespace {

using Maker = SVGElement* (*)(Document& document);

template <typename T>
SVGElement* Make(Document& document) {
  return T::Create(document);
}

struct NamedMaker {
  std::u16string_view name;
  Maker make;
};

// The names that Blink's svg_tag_names.json5 lists with a class of their own,
// sorted. Its animateColor is an SVGUnknownElement.
constexpr auto kElements = std::to_array<NamedMaker>({
    {u"a", &Make<SVGAElement>},
    {u"animate", &Make<SVGAnimateElement>},
    {u"animateMotion", &Make<SVGAnimateMotionElement>},
    {u"animateTransform", &Make<SVGAnimateTransformElement>},
    {u"circle", &Make<SVGCircleElement>},
    {u"clipPath", &Make<SVGClipPathElement>},
    {u"defs", &Make<SVGDefsElement>},
    {u"desc", &Make<SVGDescElement>},
    {u"ellipse", &Make<SVGEllipseElement>},
    {u"feBlend", &Make<SVGFEBlendElement>},
    {u"feColorMatrix", &Make<SVGFEColorMatrixElement>},
    {u"feComponentTransfer", &Make<SVGFEComponentTransferElement>},
    {u"feComposite", &Make<SVGFECompositeElement>},
    {u"feConvolveMatrix", &Make<SVGFEConvolveMatrixElement>},
    {u"feDiffuseLighting", &Make<SVGFEDiffuseLightingElement>},
    {u"feDisplacementMap", &Make<SVGFEDisplacementMapElement>},
    {u"feDistantLight", &Make<SVGFEDistantLightElement>},
    {u"feDropShadow", &Make<SVGFEDropShadowElement>},
    {u"feFlood", &Make<SVGFEFloodElement>},
    {u"feFuncA", &Make<SVGFEFuncAElement>},
    {u"feFuncB", &Make<SVGFEFuncBElement>},
    {u"feFuncG", &Make<SVGFEFuncGElement>},
    {u"feFuncR", &Make<SVGFEFuncRElement>},
    {u"feGaussianBlur", &Make<SVGFEGaussianBlurElement>},
    {u"feImage", &Make<SVGFEImageElement>},
    {u"feMerge", &Make<SVGFEMergeElement>},
    {u"feMergeNode", &Make<SVGFEMergeNodeElement>},
    {u"feMorphology", &Make<SVGFEMorphologyElement>},
    {u"feOffset", &Make<SVGFEOffsetElement>},
    {u"fePointLight", &Make<SVGFEPointLightElement>},
    {u"feSpecularLighting", &Make<SVGFESpecularLightingElement>},
    {u"feSpotLight", &Make<SVGFESpotLightElement>},
    {u"feTile", &Make<SVGFETileElement>},
    {u"feTurbulence", &Make<SVGFETurbulenceElement>},
    {u"filter", &Make<SVGFilterElement>},
    {u"foreignObject", &Make<SVGForeignObjectElement>},
    {u"g", &Make<SVGGElement>},
    {u"image", &Make<SVGImageElement>},
    {u"line", &Make<SVGLineElement>},
    {u"linearGradient", &Make<SVGLinearGradientElement>},
    {u"marker", &Make<SVGMarkerElement>},
    {u"mask", &Make<SVGMaskElement>},
    {u"metadata", &Make<SVGMetadataElement>},
    {u"mpath", &Make<SVGMPathElement>},
    {u"path", &Make<SVGPathElement>},
    {u"pattern", &Make<SVGPatternElement>},
    {u"polygon", &Make<SVGPolygonElement>},
    {u"polyline", &Make<SVGPolylineElement>},
    {u"radialGradient", &Make<SVGRadialGradientElement>},
    {u"rect", &Make<SVGRectElement>},
    {u"script", &Make<SVGScriptElement>},
    {u"set", &Make<SVGSetElement>},
    {u"stop", &Make<SVGStopElement>},
    {u"style", &Make<SVGStyleElement>},
    {u"svg", &Make<SVGSVGElement>},
    {u"switch", &Make<SVGSwitchElement>},
    {u"symbol", &Make<SVGSymbolElement>},
    {u"text", &Make<SVGTextElement>},
    {u"textPath", &Make<SVGTextPathElement>},
    {u"title", &Make<SVGTitleElement>},
    {u"tspan", &Make<SVGTSpanElement>},
    {u"use", &Make<SVGUseElement>},
    {u"view", &Make<SVGViewElement>},
});

}  // namespace

SVGElement* CreateSVGElement(Document& document,
                             const std::u16string& local_name) {
  const std::u16string_view name = local_name;
  const auto found =
      std::lower_bound(kElements.begin(), kElements.end(), name,
                       [](const NamedMaker& entry, std::u16string_view key) {
                         return entry.name < key;
                       });
  if (found != kElements.end() && found->name == name) {
    return found->make(document);
  }
  return SVGElement::Create(document, local_name);
}

}  // namespace mitto
