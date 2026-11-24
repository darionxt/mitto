// Blink's NavigationRateLimiter: at most 200 navigations of a frame in 10
// seconds, which location's setters, the history API and the Navigation API
// share, and a warning in the console for the first one past that in each
// period. Each frame keeps its own, across its documents.
//
// Upstream: third_party/blink/renderer/core/frame/navigation_rate_limiter.cc

#ifndef MITTO_DOM_NAVIGATION_RATE_LIMITER_H_
#define MITTO_DOM_NAVIGATION_RATE_LIMITER_H_

#include <optional>

#include "base/time/time.h"

namespace mitto {

class Window;

class NavigationRateLimiter {
 public:
  NavigationRateLimiter();
  NavigationRateLimiter(const NavigationRateLimiter&) = delete;
  NavigationRateLimiter& operator=(const NavigationRateLimiter&) = delete;
  ~NavigationRateLimiter();

  // Whether a navigation that `window` starts may go on, which counts it.
  // The first that may not in a period warns in `window`'s console.
  bool CanProceed(Window& window);

 private:
  base::TimeTicks period_start_;
  int count_ = 0;
  std::optional<base::TimeTicks> warned_period_start_;
};

}  // namespace mitto

#endif  // MITTO_DOM_NAVIGATION_RATE_LIMITER_H_
