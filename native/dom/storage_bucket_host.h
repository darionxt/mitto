// What a document's Storage Buckets API asks of the Session that loaded it, as
// Blink's StorageBucketManager and StorageBucket ask Chrome's browser over
// blink.mojom.BucketManagerHost and BucketHost. The Page program gives a
// document that the Session loaded over HTTP or HTTPS one, and the DOM knows
// nothing of how it reaches the Session, as with the CookieJar.
//
// Upstream: third_party/blink/public/mojom/buckets/bucket_manager_host.mojom

#ifndef MITTO_DOM_STORAGE_BUCKET_HOST_H_
#define MITTO_DOM_STORAGE_BUCKET_HOST_H_

#include <stdint.h>

#include "base/functional/callback.h"
#include "mitto/storage_bucket_messages.h"

namespace mitto {

class StorageBucketHost {
 public:
  using Answer = base::OnceCallback<void(PageStorageBucketAnswer answer)>;

  // Asks the Session, with a number of the host's for the question, and calls
  // `answer` from a task of its own once it has answered, as a Mojo reply
  // comes.
  virtual void Ask(PageStorageBucketCall call, Answer answer) = 0;

  // The Page no longer uses a bucket that the Session gave it, as dropping a
  // BucketHost remote tells the browser.
  virtual void Release(uint64_t bucket) = 0;

  // The Page has made document `document`, which takes the place of the one
  // before it: what that one asked and held goes, as its Mojo pipes go with
  // its context, and nothing of it reaches the Session from here on.
  virtual void DocumentCommitted(uint64_t document) = 0;

 protected:
  virtual ~StorageBucketHost() = default;
};

}  // namespace mitto

#endif  // MITTO_DOM_STORAGE_BUCKET_HOST_H_
