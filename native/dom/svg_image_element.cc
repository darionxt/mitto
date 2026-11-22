#include "mitto/dom/svg_image_element.h"

#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_svg_image_element.h"
#include "mitto/dom/event.h"
#include "mitto/dom/fetcher.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/image_loader.h"

namespace mitto {
namespace {

// SVGURIReference::IsKnownAttribute(): href, or xlink:href.
bool IsHrefAttribute(const QualifiedName& name) {
  return name.local_name == u"href" &&
         (!name.namespace_uri ||
          *name.namespace_uri == html_names::kXlinkNamespace);
}

}  // namespace

// static
SVGImageElement* SVGImageElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<SVGImageElement>(document);
}

SVGImageElement::SVGImageElement(Document& document)
    : SVGGraphicsElement(document, u"image"),
      image_loader_(bindings::MakeGarbageCollected<ImageLoader>(*this)) {}

const bindings::WrapperTypeInfo* SVGImageElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGImageElement::kWrapperTypeInfo;
}

void SVGImageElement::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(image_loader_);
  SVGGraphicsElement::Trace(visitor);
}

SVGAnimatedLength* SVGImageElement::X() {
  return AnimatedLength(u"x", SVGLengthMode::kWidth,
                        SVGAnimatedLength::Initial::kUnitlessZero);
}

SVGAnimatedLength* SVGImageElement::Y() {
  return AnimatedLength(u"y", SVGLengthMode::kHeight,
                        SVGAnimatedLength::Initial::kUnitlessZero);
}

SVGAnimatedLength* SVGImageElement::Width() {
  return AnimatedLength(u"width", SVGLengthMode::kWidth,
                        SVGAnimatedLength::Initial::kUnitlessZero);
}

SVGAnimatedLength* SVGImageElement::Height() {
  return AnimatedLength(u"height", SVGLengthMode::kHeight,
                        SVGAnimatedLength::Initial::kUnitlessZero);
}

v8::Local<v8::Promise> SVGImageElement::Decode(
    bindings::ExceptionState& exception_state) {
  return image_loader_->Decode(exception_state);
}

std::optional<std::u16string> SVGImageElement::ImageSourceURL() const {
  // HrefString(): href over the deprecated xlink:href.
  if (std::optional<std::u16string> href =
          GetAttributeNS(std::nullopt, u"href")) {
    return href;
  }
  return GetAttributeNS(std::u16string(html_names::kXlinkNamespace), u"href");
}

void SVGImageElement::DispatchErrorEvent() {
  // SVGImageLoader::DispatchErrorEvent().
  bindings::EventInit init;
  Event* const event = Event::Create(u"error", init);
  event->SetTrusted(true);
  DispatchEvent(event);
}

bool SVGImageElement::HaveLoadedRequiredResources() {
  return !image_loader_->HasPendingActivity();
}

void SVGImageElement::AttributeChanged(const QualifiedName& name,
                                       const std::u16string* old_value,
                                       const std::u16string* new_value) {
  SVGGraphicsElement::AttributeChanged(name, old_value, new_value);
  // ParseAttribute(): a change of the state, not of how it is written, is a
  // relevant mutation.
  if (name.namespace_uri || name.local_name != u"crossorigin") {
    return;
  }
  auto state = [](const std::u16string* value) {
    return CrossOriginAttributeFromValue(
        value ? std::optional<std::u16string>(*value) : std::nullopt);
  };
  if (state(old_value) != state(new_value)) {
    image_loader_->UpdateFromElement(ImageLoader::kUpdateIgnorePreviousError);
  }
}

void SVGImageElement::SvgAttributeChanged(const QualifiedName& name) {
  if (IsHrefAttribute(name)) {
    image_loader_->UpdateFromElement(ImageLoader::kUpdateIgnorePreviousError);
  }
}

bool SVGImageElement::IsStructurallyExternal() const {
  return ImageSourceURL().has_value();
}

void SVGImageElement::DidCloneAttributes() {
  SVGGraphicsElement::DidCloneAttributes();
  // Element::CloneAttributesFrom(): each copied attribute changes as it would
  // be set, so a copy loads its image.
  for (const Attribute& attribute : attributes()) {
    if (IsHrefAttribute(attribute.name)) {
      image_loader_->UpdateFromElement(ImageLoader::kUpdateIgnorePreviousError);
    }
  }
}

void SVGImageElement::DidMoveToNewDocument(Document& old_document) {
  image_loader_->ElementDidMoveToNewDocument();
  SVGGraphicsElement::DidMoveToNewDocument(old_document);
  image_loader_->UpdateFromElement(ImageLoader::kUpdateIgnorePreviousError);
}

}  // namespace mitto
