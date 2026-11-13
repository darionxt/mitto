// The canvas element. Its contexts are the Runtime's later work, so what it
// has is its size, which its attributes give.
//
// Upstream: third_party/blink/renderer/core/html/canvas/html_canvas_element.h
// Upstream: third_party/blink/renderer/core/html/canvas/html_canvas_element.cc

#ifndef MITTO_DOM_HTML_CANVAS_ELEMENT_H_
#define MITTO_DOM_HTML_CANVAS_ELEMENT_H_

#include <stdint.h>

#include "mitto/dom/html_element.h"

namespace mitto {

class HTMLCanvasElement final : public HTMLElement {
 public:
  static HTMLCanvasElement* Create(Document& document);

  explicit HTMLCanvasElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface: the attribute's non-negative integer, or
  // else 300 by 150, as Blink's canvas size is. Setting them throws only for a
  // canvas whose control went to an OffscreenCanvas, which none does yet.
  uint32_t Width() const;
  void SetWidth(uint32_t value, bindings::ExceptionState& exception_state);
  uint32_t Height() const;
  void SetHeight(uint32_t value, bindings::ExceptionState& exception_state);
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_CANVAS_ELEMENT_H_
