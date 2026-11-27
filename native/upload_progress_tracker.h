// Tells how the upload of a request's body goes, each 100 ms while it makes
// progress and the last report has been taken, as Chrome's network service
// tells a renderer.
//
// Upstream: services/network/upload_progress_tracker.h
// Upstream: services/network/upload_progress_tracker.cc

#ifndef MITTO_UPLOAD_PROGRESS_TRACKER_H_
#define MITTO_UPLOAD_PROGRESS_TRACKER_H_

#include <stdint.h>

#include "base/functional/callback.h"
#include "base/memory/raw_ref.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "net/base/upload_progress.h"

namespace net {
class URLRequest;
}  // namespace net

namespace mitto {

class UploadProgressTracker {
 public:
  // `report` hears of `request`'s upload, from its creation on, and `request`
  // outlives this.
  UploadProgressTracker(
      const net::URLRequest& request,
      base::RepeatingCallback<void(const net::UploadProgress&)> report);
  UploadProgressTracker(const UploadProgressTracker&) = delete;
  UploadProgressTracker& operator=(const UploadProgressTracker&) = delete;
  ~UploadProgressTracker();

  // The last report has been taken.
  void OnAckReceived() { waiting_for_ack_ = false; }

  // The response has started, or the request has ended: the last report, and
  // nothing after it.
  void OnUploadCompleted();

 private:
  void ReportIfNeeded();

  const raw_ref<const net::URLRequest> request_;
  const base::RepeatingCallback<void(const net::UploadProgress&)> report_;
  base::RepeatingTimer timer_;
  bool waiting_for_ack_ = false;
  uint64_t last_position_ = 0;
  base::TimeTicks last_ticks_;
};

}  // namespace mitto

#endif  // MITTO_UPLOAD_PROGRESS_TRACKER_H_
