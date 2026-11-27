#include "mitto/dom/page_dismissal_scope.h"

namespace mitto {
namespace {

// How many scopes are on the stack of the main thread, which alone has any.
int g_page_dismissal_scopes = 0;

}  // namespace

PageDismissalScope::PageDismissalScope() {
  ++g_page_dismissal_scopes;
}

PageDismissalScope::~PageDismissalScope() {
  --g_page_dismissal_scopes;
}

// static
bool PageDismissalScope::IsActive() {
  return g_page_dismissal_scopes > 0;
}

}  // namespace mitto
