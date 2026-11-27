#include "mitto/dom/dom_window_storage.h"

#include <iterator>
#include <string_view>
#include <utility>

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
  // an opaque origin has no storage.
  if (window.GetSecurityOrigin().opaque()) {
    if (exception_state) {
      const Document& document = window.GetDocument();
      exception_state->ThrowDOMException(
          "SecurityError",
          document.IsSandboxed(SandboxFlags::kOrigin)
              ? Storage::kAccessSandboxedMessage
          : GURL(document.UrlForBinding()).SchemeIs(url::kDataScheme)
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
  scoped_refptr<CachedStorageArea> area =
      StorageController::Get().GetCachedArea(kind, window.StorageKey(),
                                             window.DocumentId());
  if (area) {
    slot = Storage::Create(window, std::move(area));
  }
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
  const std::pair<PageStorageKind, std::string> key(kind, storage_key);
  const auto found = areas_.find(key);
  if (found != areas_.end()) {
    return found->second;
  }
  // Only the windows of frames that a script holds on to once they have gone
  // can take up the areas that the Session lets a Page bind.
  if (areas_.size() >= kMaxPageStorageAreas) {
    return nullptr;
  }
  return areas_
      .emplace(key, base::MakeRefCounted<CachedStorageArea>(
                        kind, document, *StorageBackend::Get()))
      .first->second;
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
