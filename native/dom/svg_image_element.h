// The DOM's SVGImageElement: SVG's image, whose href an ImageLoader loads as
// Blink's SVGImageLoader does, as soon as it is set, in a document or out of
// one, with decode(), and SVG's load event or error once it has loaded.
// Blink draws it; a Page does not.
//
// Upstream: third_party/blink/renderer/core/svg/svg_image_element.h
// Upstream: third_party/blink/renderer/core/svg/svg_image_element.cc
#ifndef MITTO_DOM_SVG_IMAGE_ELEMENT_H_
#define MITTO_DOM_SVG_IMAGE_ELEMENT_H_

#include <optional>
#include <string>

#include "mitto/dom/svg_elements.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings
class ImageLoader;

class SVGImageElement final : public SVGGraphicsElement {
 public:
  static SVGImageElement* Create(Document& document);

  explicit SVGImageElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  SVGAnimatedLength* X();
  SVGAnimatedLength* Y();
  SVGAnimatedLength* Width();
  SVGAnimatedLength* Height();
  v8::Local<v8::Promise> Decode(bindings::ExceptionState& exception_state);

  // What ImageLoader reads: href, or else xlink:href, null for neither.
  std::optional<std::u16string> ImageSourceURL() const;
  void DispatchErrorEvent();

  // SVGElement's: whether its image has loaded, and no update or event of
  // its loader waits.
  bool HaveLoadedRequiredResources() override;
  // Element's: a change of the crossorigin attribute's state loads the image
  // again.
  void AttributeChanged(const QualifiedName& name,
                        const std::u16string* old_value,
                        const std::u16string* new_value) override;

 protected:
  // SVGElement's: a change of href or xlink:href loads the image again.
  void SvgAttributeChanged(const QualifiedName& name) override;
  bool IsStructurallyExternal() const override;
  void DidCloneAttributes() override;
  // Node's: the image is loaded again for the new document.
  void DidMoveToNewDocument(Document& old_document) override;

 private:
  cppgc::Member<ImageLoader> image_loader_;
};

}  // namespace mitto

#endif  // MITTO_DOM_SVG_IMAGE_ELEMENT_H_
