// What gives a window its localStorage and sessionStorage: Blink's
// DOMWindowStorage, with its checks of who may have storage, and the Page's
// cached areas, which Blink's StorageController and StorageNamespace keep.
// A Page is a renderer process with one tab, so one controller keeps the
// areas of both kinds, by kind and storage key.
//
// Upstream: third_party/blink/renderer/modules/storage/dom_window_storage.cc
// Upstream: third_party/blink/renderer/modules/storage/storage_controller.cc
// Upstream: third_party/blink/renderer/modules/storage/storage_namespace.cc

#ifndef MITTO_DOM_DOM_WINDOW_STORAGE_H_
#define MITTO_DOM_DOM_WINDOW_STORAGE_H_

#include <stdint.h>

#include <map>
#include <string>
#include <utility>

#include "base/memory/scoped_refptr.h"
#include "mitto/page_messages.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class CachedStorageArea;
class Storage;
class Window;

// The window's Storage of `kind`, which `slot` keeps once made. Throws a
// SecurityError for a document of an opaque origin, and gives null for a Page
// with no Session to keep its storage, as Blink does for disabled storage.
Storage* WindowStorage(Window& window,
                       PageStorageKind kind,
                       cppgc::Member<Storage>& slot,
                       bindings::ExceptionState& exception_state);

class StorageController {
 public:
  // The Page's.
  static StorageController& Get();

  StorageController();
  StorageController(const StorageController&) = delete;
  StorageController& operator=(const StorageController&) = delete;
  ~StorageController();

  // The cached area of `kind` for `storage_key`, made the first time for the
  // window of the document numbered `document`.
  scoped_refptr<CachedStorageArea> GetCachedArea(PageStorageKind kind,
                                                 const std::string& storage_key,
                                                 uint64_t document);

 private:
  // ClearAreasIfNeeded(): past the limit of the cache, the areas that no
  // window holds go, and are bound again when one asks.
  void ClearAreasIfNeeded();

  std::map<std::pair<PageStorageKind, std::string>,
           scoped_refptr<CachedStorageArea>>
      areas_;
};

}  // namespace mitto

#endif  // MITTO_DOM_DOM_WINDOW_STORAGE_H_
