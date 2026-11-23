#include "mitto/dom/frame.h"

#include "mitto/dom/html_frame_owner_element.h"
#include "mitto/dom/window.h"

namespace mitto {

bool NavigationRateLimiter::CanProceed(Window& window) {
  const base::TimeTicks now = base::TimeTicks::Now();
  if (now - period_start_ > kPeriod) {
    period_start_ = now;
    count_ = 0;
  }
  if (count_ < kStateUpdateLimit) {
    ++count_;
    return true;
  }
  if (!warned_period_start_ || *warned_period_start_ != period_start_) {
    warned_period_start_ = period_start_;
    window.AddConsoleMessage(
        ConsoleLevel::kWarning,
        "Throttling navigation to prevent the browser from hanging. See "
        "https://crbug.com/1038223. Command line switch "
        "--disable-ipc-flooding-protection can be used to bypass the "
        "protection");
  }
  return false;
}

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

bool Frame::IsSandboxed() const {
  for (const Frame* frame = this; frame; frame = frame->Parent()) {
    const HTMLFrameOwnerElement* const owner = frame->Owner();
    if (owner && owner->FindAttribute(u"sandbox")) {
      return true;
    }
  }
  return false;
}

}  // namespace mitto
