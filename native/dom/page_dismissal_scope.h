// Blink's PageDismissalScope: while one is on the stack, the events of a page
// that is left run, beforeunload, pagehide, visibilitychange or unload of any
// document of the Page, during which no synchronous XMLHttpRequest may be sent.
//
// Upstream: third_party/blink/renderer/core/frame/page_dismissal_scope.h
// Upstream: third_party/blink/renderer/core/frame/page_dismissal_scope.cc

#ifndef MITTO_DOM_PAGE_DISMISSAL_SCOPE_H_
#define MITTO_DOM_PAGE_DISMISSAL_SCOPE_H_

#include "base/memory/stack_allocated.h"

namespace mitto {

class PageDismissalScope {
  STACK_ALLOCATED();

 public:
  PageDismissalScope();
  PageDismissalScope(const PageDismissalScope&) = delete;
  PageDismissalScope& operator=(const PageDismissalScope&) = delete;
  ~PageDismissalScope();

  // On the main thread.
  static bool IsActive();
};

}  // namespace mitto

#endif  // MITTO_DOM_PAGE_DISMISSAL_SCOPE_H_
