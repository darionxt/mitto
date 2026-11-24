#include "mitto/dom/dom_window_storage.h"

#include <iterator>
#include <string_view>

#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/dom/cached_storage_area.h"
#include "mitto/dom/document.h"
#include "mitto/dom/storage.h"
#include "mitto/dom/storage_backend.h"
#include "mitto/dom/window.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace mitto {

namespace {

// kStorageControllerTotalCacheLimitInBytes: what the Page's cached areas may
// hold before those that no window holds go.
constexpr size_t kTotalCacheLimitInBytes = 5 * 1024 * 1024;

// How many areas the Page keeps cached before those that no window holds go,
// beside Blink's limit of bytes: half as many as the Session lets a Page
// bind, where Chrome's browser takes any number. An area that goes loads
// again from the Session when a window reaches it, so no script can tell.
constexpr size_t kMaxCachedAreas = kMaxPageStorageAreas / 2;

}  // namespace

Storage* WindowStorage(Window& window,
                       PageStorageKind kind,
                       cppgc::Member<Storage>& slot,
                       bindings::ExceptionState* exception_state) {
  // GetOrCreateLocalStorage(): a window that has lost its frame, as a removed
  // frame's that a script still holds, whose context has gone, has none, not
  // even one it made. The Page program given a script has a window with no
  // frame, whose context remains.
  if (!window.HasContext()) {
    return nullptr;
  }
  // GetOrCreateLocalStorage() and GetOrCreateSessionStorage(): a document of
  // an opaque origin has no storage. No document of a Page is sandboxed yet.
  if (window.GetSecurityOrigin().opaque()) {
    if (exception_state) {
      const GURL url(base::UTF16ToUTF8(window.GetDocument().UrlForBinding()));
      exception_state->ThrowDOMException("SecurityError",
                                         url.SchemeIs(url::kDataScheme)
                                             ? Storage::kAccessDataMessage
                                             : Storage::kAccessDeniedMessage);
    }
    return nullptr;
  }
  if (slot) {
    return slot.Get();
  }
  // As for a frame whose storage is disabled: no Session keeps it.
  if (!StorageBackend::Get()) {
    return nullptr;
  }
  // Documents of one origin in two partitions, as a top-level document's and
  // a frame's of the same origin under another site, have an area each.
  slot = Storage::Create(
      window, StorageController::Get().GetCachedArea(kind, window.StorageKey(),
                                                     window.DocumentId()));
  return slot.Get();
}

Storage* Window::LocalStorage(bindings::ExceptionState& exception_state) {
  return WindowStorage(*this, PageStorageKind::kLocal, local_storage_,
                       &exception_state);
}

Storage* Window::SessionStorage(bindings::ExceptionState& exception_state) {
  return WindowStorage(*this, PageStorageKind::kSession, session_storage_,
                       &exception_state);
}

// DOMWindowStorageController::DidAddEventListener(): a window hears of storage
// events once it has its Storage objects, which a listener of them makes.
void Window::ListenersChanged(std::u16string_view type) {
  if (type == u"storage" && HasEventListeners(type)) {
    WindowStorage(*this, PageStorageKind::kLocal, local_storage_,
                  /*exception_state=*/nullptr);
    WindowStorage(*this, PageStorageKind::kSession, session_storage_,
                  /*exception_state=*/nullptr);
  }
}

// static
StorageController& StorageController::Get() {
  static base::NoDestructor<StorageController> controller;
  return *controller;
}

StorageController::StorageController() = default;
StorageController::~StorageController() = default;

scoped_refptr<CachedStorageArea> StorageController::GetCachedArea(
    PageStorageKind kind,
    const std::string& storage_key,
    uint64_t document) {
  ClearAreasIfNeeded();
  scoped_refptr<CachedStorageArea>& area = areas_[{kind, storage_key}];
  if (!area) {
    area = base::MakeRefCounted<CachedStorageArea>(kind, document,
                                                   *StorageBackend::Get());
  }
  return area;
}

void StorageController::ClearAreasIfNeeded() {
  size_t total = 0;
  for (const auto& [key, area] : areas_) {
    total += area->memory_used();
  }
  if (total < kTotalCacheLimitInBytes && areas_.size() < kMaxCachedAreas) {
    return;
  }
  std::erase_if(areas_,
                [](const auto& entry) { return entry.second->HasOneRef(); });
}

}  // namespace mitto
