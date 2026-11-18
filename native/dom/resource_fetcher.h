// A document's fetches for its elements, as Blink's ResourceFetcher makes
// them: each request's priority, data: URLs answered in the Page as Blink
// answers them in the renderer, and the document's resources, which a
// request of the same URL reuses as Blink's rules allow, such as the HTML
// Standard's list of available images.
//
// Upstream: third_party/blink/renderer/platform/loader/fetch/resource_fetcher.cc
// Upstream: third_party/blink/renderer/platform/loader/fetch/resource_request_utils.cc

#ifndef MITTO_DOM_RESOURCE_FETCHER_H_
#define MITTO_DOM_RESOURCE_FETCHER_H_

#include <optional>

#include "mitto/dom/fetcher.h"
#include "mitto/dom/heap_vector.h"
#include "mitto/dom/resource.h"
#include "url/gurl.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class Document;

// Blink's FetchParameters::SpeculativePreloadType: whether the preload
// scanner made the request, and from where.
enum class SpeculativePreloadType {
  kNotSpeculative,
  kInDocument,
  kInserted,
};

// What an element asks for, as Blink's FetchParameters holds it.
struct FetchParameters {
  FetchParameters();
  FetchParameters(const FetchParameters&);
  FetchParameters& operator=(const FetchParameters&);
  ~FetchParameters();

  GURL url;
  ResourceType type = ResourceType::kImage;
  // The request's mode and credentials mode: no-cors and include unless a
  // crossorigin attribute asks for cors.
  FetchMode mode = FetchMode::kNoCors;
  FetchCredentialsMode credentials = FetchCredentialsMode::kInclude;
  FetchReferrerPolicy referrer_policy = FetchReferrerPolicy::kDefault;
  FetchPriorityHint priority_hint = FetchPriorityHint::kAuto;
  // FetchParameters::kLazyLoad, as an async script or a lazy image has.
  bool lazy = false;
  // The element's width and height, which say whether an image is small.
  std::optional<float> resource_width;
  std::optional<float> resource_height;
  SpeculativePreloadType speculative_preload_type =
      SpeculativePreloadType::kNotSpeculative;
  bool is_link_preload = false;
  bool render_blocking = false;
  // Whether its fetch holds the document's load event back, as one of a
  // load-blocking type does in Blink, but for an image that was lazy, which
  // FetchParameters::SetLazyImageNonBlocking() lets go.
  bool load_blocking = true;
};

class ResourceFetcher final : public cppgc::GarbageCollected<ResourceFetcher> {
 public:
  explicit ResourceFetcher(Document& document);
  ResourceFetcher(const ResourceFetcher&) = delete;
  ResourceFetcher& operator=(const ResourceFetcher&) = delete;
  ~ResourceFetcher();

  void Trace(cppgc::Visitor* visitor) const;

  // Blink's RequestResource(): the document's resource of the URL if Blink
  // would reuse it, or a new one, which starts loading at once. A data: URL
  // loads before it returns. Null for a request that never starts, such as
  // one of a document without a Session, or a data: URL that does not parse.
  Resource* RequestResource(const FetchParameters& params);

  // The document's resource of `url`, its fragment aside, if it has one.
  Resource* CachedResource(const GURL& url) const;

  // Blink's ComputeLoadPriority() of a request, which counts the images that
  // the preload scanner found.
  FetchPriority ComputeLoadPriority(const FetchParameters& params);

 private:
  class ResourceLoader;

  // Whether Blink's DetermineRevalidationPolicy() uses `existing` for
  // `params`.
  bool CanUse(const Resource& existing, const FetchParameters& params) const;
  void StartLoad(Resource& resource, const FetchParameters& params);
  // The fetch of `resource` has ended, and lets the load event go if it held
  // it back.
  void LoadFinished(Resource& resource);
  // Blink's AllClientsAndObserversRemoved(): a load that nothing uses is
  // cancelled from a task, unless something uses it by then.
  void ScheduleCancel(Resource& resource);
  void CancelLoad(Resource& resource);

  friend class Resource;

  cppgc::WeakMember<Document> document_;
  // The document's resources, Blink's cached_resources_map_, newest last.
  HeapVector<Resource> resources_;
  // The resources whose fetch holds the document's load event back.
  HeapVector<Resource> blocking_;
  // Whether the preload scanner has asked for an image, after which it
  // lowers late scripts and style sheets.
  bool image_fetched_ = false;
  int boosted_image_count_ = 0;
};

}  // namespace mitto

#endif  // MITTO_DOM_RESOURCE_FETCHER_H_
