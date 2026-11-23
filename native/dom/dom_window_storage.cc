#include "mitto/dom/dom_window_storage.h"

#include <iterator>
#include <vector>

#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/dom/cached_storage_area.h"
#include "mitto/dom/document.h"
#include "mitto/dom/frame.h"
#include "mitto/dom/storage.h"
#include "mitto/dom/storage_backend.h"
#include "mitto/dom/window.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
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

// net::SchemefulSite of `origin`: its scheme and registrable domain, or its
// host where it has none, and an opaque origin its own.
std::string SiteOf(const url::Origin& origin) {
  if (origin.opaque()) {
    return origin.GetDebugString();
  }
  const std::string domain =
      net::registry_controlled_domains::GetDomainAndRegistry(
          origin, net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
  return base::StrCat(
      {origin.scheme(), "://", domain.empty() ? origin.host() : domain});
}

// The storage key that the Session gives the window's document of `origin`,
// as RenderFrameHostImpl::CalculateStorageKey() has it with third-party
// storage partitioning: the origin, the site of the top-level document, and
// whether a document from the window's up to it is of another site.
std::string StorageKeyOf(const Window& window, const url::Origin& origin) {
  std::vector<url::Origin> documents = {origin};
  const Frame* const frame = window.GetFrame();
  for (const Frame* ancestor = frame ? frame->Parent() : nullptr; ancestor;
       ancestor = ancestor->Parent()) {
    if (const Window* const ancestor_window = ancestor->DomWindow()) {
      documents.push_back(ancestor_window->GetSecurityOrigin());
    }
  }
  const std::string top_site = SiteOf(documents.back());
  bool cross_site = false;
  for (const url::Origin& document : documents) {
    cross_site = cross_site || SiteOf(document) != top_site;
  }
  return base::StrCat({origin.Serialize(), " ", top_site,
                       cross_site ? " cross-site" : " same-site"});
}

}  // namespace

Storage* WindowStorage(Window& window,
                       PageStorageKind kind,
                       cppgc::Member<Storage>& slot,
                       bindings::ExceptionState& exception_state) {
  const GURL url(base::UTF16ToUTF8(window.GetDocument().UrlForBinding()));
  const url::Origin origin = url::Origin::Create(url);
  // GetOrCreateLocalStorage() and GetOrCreateSessionStorage(): a document of
  // an opaque origin has no storage. No document of a Page is sandboxed yet.
  if (origin.opaque()) {
    exception_state.ThrowDOMException("SecurityError",
                                      url.SchemeIs(url::kDataScheme)
                                          ? Storage::kAccessDataMessage
                                          : Storage::kAccessDeniedMessage);
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
      window, StorageController::Get().GetCachedArea(
                  kind, StorageKeyOf(window, origin), window.DocumentId()));
  return slot.Get();
}

Storage* Window::LocalStorage(bindings::ExceptionState& exception_state) {
  return WindowStorage(*this, PageStorageKind::kLocal, local_storage_,
                       exception_state);
}

Storage* Window::SessionStorage(bindings::ExceptionState& exception_state) {
  return WindowStorage(*this, PageStorageKind::kSession, session_storage_,
                       exception_state);
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
