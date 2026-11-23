#include "mitto/dom/frame.h"

namespace mitto {

Frame& Frame::Top() {
  Frame* frame = this;
  while (Frame* const parent = frame->Parent()) {
    frame = parent;
  }
  return *frame;
}

Frame* Frame::ScopedChild(std::u16string_view name) const {
  if (name.empty()) {
    return nullptr;
  }
  for (size_t index = 0; index < ChildCount(); ++index) {
    Frame* const child = ChildAt(index);
    if (child->Name() == name) {
      return child;
    }
  }
  return nullptr;
}

bool Frame::IsDescendantOf(const Frame& other) const {
  for (const Frame* frame = this; frame; frame = frame->Parent()) {
    if (frame == &other) {
      return true;
    }
  }
  return false;
}

}  // namespace mitto
