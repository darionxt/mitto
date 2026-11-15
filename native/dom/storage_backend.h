// What a Page's localStorage and sessionStorage ask of the Session that keeps
// them: Blink's StorageArea interface and its binding by the browser, without
// Mojo. The Page program gives one for a Page that a Session opened. Without
// one, as for the Page program given a script, whose document is about:blank's
// and so of an opaque origin, a window has no storage.
//
// Each area that the Page binds and each observer of it has a number that the
// Page picks and never uses again. The Session tells an observer of each change
// to its area, and a new observer takes the old one's place, as a new Mojo pipe
// would: the Page drops what comes for an observer that it replaced.
//
// Upstream: third_party/blink/public/mojom/dom_storage/storage_area.mojom

#ifndef MITTO_DOM_STORAGE_BACKEND_H_
#define MITTO_DOM_STORAGE_BACKEND_H_

#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

#include "mitto/page_messages.h"

namespace mitto {

class CachedStorageArea;

class StorageBackend {
 public:
  // The Page's, or null.
  static StorageBackend* Get();
  static void Set(StorageBackend* backend);

  // On the main thread, all of them.

  // A number for a new observer.
  virtual uint64_t NewObserver() = 0;
  // Binds an area of `kind` for the storage key of `document`, the ID of the
  // document whose window first reaches it, with its first observer, whose
  // events go to `area` on the main thread until another takes its place.
  // Returns the area's number.
  virtual uint64_t Bind(PageStorageKind kind,
                        uint64_t document,
                        uint64_t observer,
                        CachedStorageArea* area) = 0;
  // What the area holds, for which this waits, as Blink's GetAll() does.
  // `observer` takes the place of the area's observer.
  virtual std::vector<std::pair<std::string, std::string>> GetAll(
      uint64_t area,
      uint64_t observer) = 0;
  // A change to the area. A kStorageDeleteAll with an observer other than 0
  // puts it in the place of the area's.
  virtual void Mutate(PageMessage type, PageStorageMutation mutation) = 0;
  virtual void Unbind(uint64_t area) = 0;

 protected:
  virtual ~StorageBackend() = default;
};

}  // namespace mitto

#endif  // MITTO_DOM_STORAGE_BACKEND_H_
