#include "mitto/dom/frame.h"

#include <algorithm>
#include <string_view>
#include <vector>

#include "base/hash/hash.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/document.h"
#include "mitto/dom/html_frame_owner_element.h"
#include "mitto/dom/remote_window.h"
#include "mitto/dom/window.h"

namespace mitto {

Frame::Frame() = default;
Frame::~Frame() = default;

RemoteWindow* Frame::RemoteWindowFor(const RendererProcess& viewer) {
  std::erase_if(remote_windows_,
                [](const cppgc::WeakPersistent<RemoteWindow>& window) {
                  return !window;
                });
  for (const cppgc::WeakPersistent<RemoteWindow>& window : remote_windows_) {
    if (IsInSameProcess(window->Viewer(), viewer)) {
      return window.Get();
    }
  }
  auto* const window =
      bindings::MakeGarbageCollected<RemoteWindow>(viewer, DomWindow());
  remote_windows_.emplace_back(window);
  return window;
}

void Frame::DidShowDocument(Window& window) {
  // A document of the parent's origin shares the parent's quota of
  // fetchLater(), so the frame's element reserves none for it, as Document's
  // constructor has HTMLFrameOwnerElement::MaybeClearDeferredFetchPolicy() do.
  // Upstream: third_party/blink/renderer/core/fetch/fetch_later_util.cc
  if (HTMLFrameOwnerElement* const owner = Owner()) {
    const Window* const parent_window =
        Parent() ? Parent()->DomWindow() : nullptr;
    if (parent_window && window.GetSecurityOrigin().IsSameOriginWith(
                             parent_window->GetSecurityOrigin())) {
      owner->SetDeferredFetchPolicy(DeferredFetchPolicy::kDisabled);
    }
  }
  const RendererProcess process = window.Process();
  std::erase_if(remote_windows_,
                [&](const cppgc::WeakPersistent<RemoteWindow>& remote_window) {
                  if (!remote_window) {
                    return true;
                  }
                  const bool replaced =
                      IsInSameProcess(remote_window->Viewer(), process);
                  remote_window->SetCurrentWindow(replaced ? nullptr : &window);
                  return replaced;
                });
}

Frame* Frame::Opener() const {
  // A frame that has gone, whose window has lost it, opens nothing any more.
  return opener_ && opener_->DomWindow() &&
                 opener_->DomWindow()->GetFrame() == opener_.get()
             ? opener_.get()
             : nullptr;
}

void Frame::SetOpener(Frame* opener) {
  opener_ = opener ? opener->opener_factory_.GetWeakPtr() : nullptr;
}

bool Frame::AddPolicyMessage(const std::string& text, bool discard_duplicate) {
  constexpr size_t kMaxMessages = 1000;
  const size_t hash = base::FastHash(text);
  if (discard_duplicate && std::ranges::contains(policy_messages_, hash)) {
    return false;
  }
  if (policy_messages_.size() == kMaxMessages) {
    policy_messages_.pop_front();
  }
  policy_messages_.push_back(hash);
  return true;
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

SandboxFlags Frame::PendingSandboxFlags() const {
  const HTMLFrameOwnerElement* const owner = Owner();
  const Frame* const parent = Parent();
  const Window* const window = parent ? parent->DomWindow() : nullptr;
  return (owner ? owner->GetSandboxFlags() : SandboxFlags::kNone) |
         (window && window->HasDocument()
              ? window->GetDocument().GetSandboxFlags()
              : SandboxFlags::kNone);
}

}  // namespace mitto
