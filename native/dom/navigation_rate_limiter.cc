#include "mitto/dom/navigation_rate_limiter.h"

#include "mitto/dom/execution_context.h"
#include "mitto/dom/window.h"

namespace mitto {
namespace {

constexpr base::TimeDelta kPeriod = base::Seconds(10);
constexpr int kStateUpdateLimit = 200;

}  // namespace

NavigationRateLimiter::NavigationRateLimiter() = default;
NavigationRateLimiter::~NavigationRateLimiter() = default;

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

}  // namespace mitto
