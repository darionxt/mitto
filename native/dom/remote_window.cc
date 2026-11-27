#include "mitto/dom/remote_window.h"

#include <utility>

#include "mitto/dom/frame.h"
#include "mitto/dom/window.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "url/scheme_host_port.h"

namespace mitto {

bool IsInSameProcess(const RendererProcess& a, const RendererProcess& b) {
  const url::SchemeHostPort& of_a = a.origin.GetTupleOrPrecursorTupleIfOpaque();
  const url::SchemeHostPort& of_b = b.origin.GetTupleOrPrecursorTupleIfOpaque();
  if (a.isolated_by_document_policy || b.isolated_by_document_policy) {
    return a.isolated_by_document_policy == b.isolated_by_document_policy &&
           a.sandboxed == b.sandboxed && of_a == of_b;
  }
  if (a.sandboxed || b.sandboxed) {
    return a.sandboxed == b.sandboxed && of_a == of_b;
  }
  if (of_a.scheme() != of_b.scheme()) {
    return false;
  }
  return of_a.host() == of_b.host() ||
         net::registry_controlled_domains::SameDomainOrHost(
             of_a.GetURL(), of_b.GetURL(),
             net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
}

// static
RemoteWindow* RemoteWindow::From(const Window& window,
                                 const RendererProcess& viewer) {
  Frame* const frame = window.GetFrame();
  return frame && !IsInSameProcess(window.Process(), viewer)
             ? frame->RemoteWindowFor(viewer)
             : nullptr;
}

RemoteWindow::RemoteWindow(RendererProcess viewer, Window* window)
    : viewer_(std::move(viewer)), window_(window) {}

RemoteWindow::~RemoteWindow() = default;

void RemoteWindow::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(window_);
}

}  // namespace mitto
