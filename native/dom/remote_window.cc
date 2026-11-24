#include "mitto/dom/remote_window.h"

#include <utility>

#include "mitto/dom/frame.h"
#include "mitto/dom/window.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "url/scheme_host_port.h"

namespace mitto {

bool IsInSameProcess(const url::Origin& a, const url::Origin& b) {
  const url::SchemeHostPort& of_a = a.GetTupleOrPrecursorTupleIfOpaque();
  const url::SchemeHostPort& of_b = b.GetTupleOrPrecursorTupleIfOpaque();
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
                                 const url::Origin& viewer) {
  Frame* const frame = window.GetFrame();
  return frame && !IsInSameProcess(window.GetSecurityOrigin(), viewer)
             ? frame->RemoteWindowFor(viewer)
             : nullptr;
}

RemoteWindow::RemoteWindow(url::Origin viewer) : viewer_(std::move(viewer)) {}

RemoteWindow::~RemoteWindow() = default;

}  // namespace mitto
