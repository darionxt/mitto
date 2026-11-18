// The img element, which new Image() makes too. It picks its source from
// srcset, sizes and its picture's source elements as Blink does, and loads
// it through the document's ResourceFetcher with Blink's ImageLoader. Without
// layout, its width and height are those its attributes give, or else its
// natural size.
//
// Upstream: third_party/blink/renderer/core/html/html_image_element.h
// Upstream: third_party/blink/renderer/core/html/html_image_element.cc

#ifndef MITTO_DOM_HTML_IMAGE_ELEMENT_H_
#define MITTO_DOM_HTML_IMAGE_ELEMENT_H_

#include <stdint.h>

#include <optional>
#include <string>

#include "mitto/dom/fetcher.h"
#include "mitto/dom/html_element.h"
#include "mitto/dom/html_srcset_parser.h"
#include "mitto/dom/image_loader.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/v8-forward.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class HTMLSourceElement;

class HTMLImageElement final : public HTMLElement {
 public:
  static HTMLImageElement* Create(Document& document);
  // new Image(width, height), in the current window's document.
  static HTMLImageElement* CreateForJSConstructor(
      std::optional<uint32_t> width,
      std::optional<uint32_t> height);

  explicit HTMLImageElement(Document& document);
  ~HTMLImageElement() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface. Blink's width() and height() of an element
  // with no layout object: the attribute's non-negative integer, or else the
  // natural size, which is 0 before its source loads.
  uint32_t Width() const;
  void SetWidth(uint32_t value);
  uint32_t Height() const;
  void SetHeight(uint32_t value);
  // The natural size, divided by the density that srcset gave its source.
  uint32_t NaturalWidth() const;
  uint32_t NaturalHeight() const;
  bool Complete() const;
  // The current request's URL: "" until it is available or broken.
  std::u16string CurrentSrc() const;
  v8::Local<v8::Promise> Decode(bindings::ExceptionState& exception_state);
  // HTMLElement's: an image is draggable unless its attribute says "false".
  bool Draggable() const override;
  // Its ID is a named item of its document only while it has a name.
  NamedItemType GetNamedItemType() const override;

  // What ImageLoader reads: the URL srcset chose, or else src, null for
  // neither.
  std::optional<std::u16string> ImageSourceURL() const;
  CrossOriginAttribute GetCrossOriginAttribute() const;
  FetchReferrerPolicy ReferrerPolicyForFetch() const;
  FetchPriorityHint GetFetchPriorityHint() const;
  void DispatchLoadEvent();
  void DispatchErrorEvent();
  bool HasLazyLoadingAttribute() const;

  // Picks the source anew, and loads it unless the viewport's change gave
  // the same URL.
  void SelectSourceURL(ImageLoader::UpdateFromElementBehavior behavior);

  // Node:
  InsertionNotificationRequest InsertedInto(Node& insertion_point) override;
  void RemovedFrom(Node& insertion_point) override;
  void DidMoveToNewDocument(Document& old_document) override;
  // Element:
  void AttributeChanged(const QualifiedName& name,
                        const std::u16string* old_value,
                        const std::u16string* new_value) override;

 private:
  ImageCandidate FindBestFitImageFromPictureParent();
  // What tells srcset's picker that a candidate needs no download.
  SrcsetImageAvailable AvailableImages();
  // The source size that `element`'s sizes gives, an img's or a source's.
  float SourceSize(Element& element);
  void SetBestFitURLAndDPRFromImageCandidate(const ImageCandidate& candidate);
  // Whether it allows auto-sizes: lazy, and sizes auto or "auto,...".
  bool AllowAutoSizes() const;

  cppgc::Member<ImageLoader> image_loader_;
  // The source element whose srcset gave the source, if any.
  cppgc::Member<HTMLSourceElement> source_;
  std::optional<std::u16string> best_fit_image_url_;
  // One over the density of the candidate that srcset chose.
  float image_device_pixel_ratio_ = 1;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_IMAGE_ELEMENT_H_
