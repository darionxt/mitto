// The img element, which new Image() makes too. Fetching its source is the
// Runtime's later work, so it has no natural size, and without layout its
// width and height are those its attributes give.
//
// Upstream: third_party/blink/renderer/core/html/html_image_element.h
// Upstream: third_party/blink/renderer/core/html/html_image_element.cc

#ifndef MITTO_DOM_HTML_IMAGE_ELEMENT_H_
#define MITTO_DOM_HTML_IMAGE_ELEMENT_H_

#include <stdint.h>

#include <optional>

#include "mitto/dom/html_element.h"

namespace mitto {

class HTMLImageElement final : public HTMLElement {
 public:
  static HTMLImageElement* Create(Document& document);
  // new Image(width, height), in the current window's document.
  static HTMLImageElement* CreateForJSConstructor(
      std::optional<uint32_t> width,
      std::optional<uint32_t> height);

  explicit HTMLImageElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface. Blink's width() and height() of an element
  // with no layout object: the attribute's non-negative integer, or else the
  // natural size, which is 0 before its source loads.
  uint32_t Width() const;
  void SetWidth(uint32_t value);
  uint32_t Height() const;
  void SetHeight(uint32_t value);
  // HTMLElement's: an image is draggable unless its attribute says "false".
  bool Draggable() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_IMAGE_ELEMENT_H_
