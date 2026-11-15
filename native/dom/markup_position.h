// A place in a document's markup, as Blink's TextPosition counts it.

#ifndef MITTO_DOM_MARKUP_POSITION_H_
#define MITTO_DOM_MARKUP_POSITION_H_

namespace mitto {

// From zero, in UTF-16 code units.
struct MarkupPosition {
  int line = 0;
  int column = 0;
};

}  // namespace mitto

#endif  // MITTO_DOM_MARKUP_POSITION_H_
