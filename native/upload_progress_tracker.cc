#include "mitto/upload_progress_tracker.h"

#include <utility>

#include "base/location.h"
#include "net/url_request/url_request.h"

namespace mitto {
namespace {

// network::UploadProgressTracker's interval, and what it reports at once: the
// end, more than half a percent, or anything after a second.
constexpr base::TimeDelta kUploadProgressInterval = base::Milliseconds(100);
constexpr uint64_t kHalfPercentIncrements = 200;
constexpr base::TimeDelta kOneSecond = base::Seconds(1);

}  // namespace

UploadProgressTracker::UploadProgressTracker(
    const net::URLRequest& request,
    base::RepeatingCallback<void(const net::UploadProgress&)> report)
    : request_(request), report_(std::move(report)) {
  timer_.Start(FROM_HERE, kUploadProgressInterval, this,
               &UploadProgressTracker::ReportIfNeeded);
}

UploadProgressTracker::~UploadProgressTracker() = default;

void UploadProgressTracker::OnUploadCompleted() {
  waiting_for_ack_ = false;
  ReportIfNeeded();
  timer_.Stop();
}

void UploadProgressTracker::ReportIfNeeded() {
  if (waiting_for_ack_) {
    return;
  }
  const net::UploadProgress progress = request_->GetUploadProgress();
  // Nothing to upload, or no progress since the last time, as when a redirect
  // or a retry starts it again.
  if (progress.size() == 0 || progress.position() <= last_position_) {
    return;
  }
  const base::TimeTicks now = base::TimeTicks::Now();
  if (progress.position() == progress.size() ||
      progress.position() - last_position_ >
          progress.size() / kHalfPercentIncrements ||
      now - last_ticks_ > kOneSecond) {
    last_ticks_ = now;
    last_position_ = progress.position();
    waiting_for_ack_ = true;
    report_.Run(progress);
  }
}

}  // namespace mitto
