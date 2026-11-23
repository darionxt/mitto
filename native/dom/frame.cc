#include "mitto/dom/frame.h"

#include <string_view>

#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "mitto/dom/html_frame_owner_element.h"
#include "mitto/dom/window.h"

namespace mitto {

namespace {

// Whether a sandbox attribute allows both scripts and the frame's origin, as
// Blink's ParseSandboxPolicy() reads its tokens: split at HTML whitespace,
// in any case.
bool AllowsScriptsAndOrigin(std::u16string_view sandbox) {
  bool scripts = false;
  bool origin = false;
  for (const std::u16string_view token :
       base::SplitStringPiece(sandbox, u"\t\n\f\r ", base::KEEP_WHITESPACE,
                              base::SPLIT_WANT_NONEMPTY)) {
    scripts =
        scripts || base::EqualsCaseInsensitiveASCII(token, u"allow-scripts");
    origin =
        origin || base::EqualsCaseInsensitiveASCII(token, u"allow-same-origin");
  }
  return scripts && origin;
}

}  // namespace

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
    const std::u16string* const sandbox =
        owner ? owner->FindAttribute(u"sandbox") : nullptr;
    if (sandbox && !AllowsScriptsAndOrigin(*sandbox)) {
      return true;
    }
  }
  return false;
}

}  // namespace mitto
