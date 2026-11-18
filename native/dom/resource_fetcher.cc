#include "mitto/dom/resource_fetcher.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/memory/raw_ptr.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/data_url.h"
#include "mitto/dom/document.h"
#include "mitto/dom/window.h"
#include "mitto/encoding/content_type.h"
#include "mitto/header_rules.h"
#include "url/url_constants.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {
namespace {

// net::ERR_INVALID_URL, which the Page program, with no //net, cannot name.
constexpr int kInvalidUrl = -300;

// Blink's kSmallImageMaxSize: an image of at most this many square pixels
// by its attributes is small, and gets no boost.
constexpr float kSmallImageMaxSize = 10000;
// kBoostedImageTarget: how many images the preload scanner finds that get a
// boost.
constexpr int kBoostedImageTarget = 5;

// TypeToPriority().
FetchPriority TypeToPriority(ResourceType type) {
  switch (type) {
    case ResourceType::kImage:
      return FetchPriority::kLow;
  }
}

// AdjustPriorityWithPriorityHintAndRenderBlocking().
FetchPriority AdjustPriorityWithPriorityHintAndRenderBlocking(
    FetchPriority priority,
    FetchPriorityHint hint,
    bool render_blocking) {
  switch (hint) {
    case FetchPriorityHint::kAuto:
      break;
    case FetchPriorityHint::kHigh:
      priority = std::max(priority, FetchPriority::kHigh);
      break;
    case FetchPriorityHint::kLow:
      priority = std::min(priority, FetchPriority::kLow);
      break;
  }
  if (render_blocking) {
    priority = std::max(priority, FetchPriority::kHigh);
  }
  return priority;
}

// MemoryCache::RemoveFragmentIdentifierIfNeeded().
GURL WithoutFragment(const GURL& url) {
  if (!url.has_ref()) {
    return url;
  }
  GURL::Replacements no_fragment;
  no_fragment.ClearRef();
  return url.ReplaceComponents(no_fragment);
}

FetchDestination DestinationOf(ResourceType type) {
  switch (type) {
    case ResourceType::kImage:
      return FetchDestination::kImage;
  }
}

}  // namespace

// What a resource's fetch hears, which keeps the resource until it ends, and
// the document's load event back while it blocks it, as Blink's loaders_ do.
class ResourceFetcher::ResourceLoader final : public FetchClient {
 public:
  ResourceLoader(Resource& resource, ResourceFetcher* blocking)
      : resource_(&resource), blocking_(blocking) {}

  ResourceLoader(const ResourceLoader&) = delete;
  ResourceLoader& operator=(const ResourceLoader&) = delete;

  ~ResourceLoader() override = default;

  // FetchClient:
  void OnStarted(FetchController& controller) override {
    controller_ = &controller;
  }
  void OnResponseStarted(const FetchResponseHead& head) override {
    resource_->ResponseReceived(
        head.status,
        GetMimeTypeAndCharset(HeaderField(head.headers, "content-type"))
            .mime_type);
  }
  void OnResponseBody(std::string bytes) override {
    resource_->AppendData(bytes);
    // A resource's body is taken as it comes.
    controller_->TookBody(bytes.size());
  }
  void OnResponseEnded(int net_error) override {
    resource_->Finish(net_error);
    if (ResourceFetcher* const fetcher = blocking_.Get()) {
      blocking_ = nullptr;
      fetcher->BlockingLoadFinished();
    }
  }

 private:
  const cppgc::Persistent<Resource> resource_;
  cppgc::WeakPersistent<ResourceFetcher> blocking_;
  raw_ptr<FetchController> controller_ = nullptr;
};

FetchParameters::FetchParameters() = default;
FetchParameters::FetchParameters(const FetchParameters&) = default;
FetchParameters& FetchParameters::operator=(const FetchParameters&) = default;
FetchParameters::~FetchParameters() = default;

ResourceFetcher::ResourceFetcher(Document& document) : document_(&document) {}

ResourceFetcher::~ResourceFetcher() = default;

void ResourceFetcher::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(document_);
  visitor->Trace(resources_);
}

Resource* ResourceFetcher::CachedResource(const GURL& url) const {
  const GURL key = WithoutFragment(url);
  for (size_t i = resources_.size(); i > 0; --i) {
    Resource* const resource = resources_[i - 1];
    if (WithoutFragment(resource->Url()) == key) {
      return resource;
    }
  }
  return nullptr;
}

bool ResourceFetcher::CanUse(const Resource& existing,
                             const FetchParameters& params) const {
  if (existing.GetType() != params.type) {
    return false;
  }
  // A data: URL's resource is static.
  if (existing.Url().SchemeIs(url::kDataScheme)) {
    return true;
  }
  // Resource::CanReuse(): the same mode and credentials mode.
  if (existing.Mode() != params.mode ||
      existing.Credentials() != params.credentials) {
    return false;
  }
  // During the initial load, one request for a URL, even one that failed.
  const Document* const document = document_.Get();
  if (document && !document->LoadEventFinished()) {
    return true;
  }
  if (existing.IsLoading()) {
    return true;
  }
  if (existing.ErrorOccurred()) {
    return false;
  }
  // Images are reused without cache validation, even with no-store, as
  // kReuseNoStoreImageOnSameSrcReassignment has it.
  return existing.GetType() == ResourceType::kImage;
}

Resource* ResourceFetcher::RequestResource(const FetchParameters& params) {
  Document* const document = document_.Get();
  Window* const window = document ? document->DefaultView() : nullptr;
  if (!window || !params.url.is_valid()) {
    return nullptr;
  }
  Resource* const existing = CachedResource(params.url);
  if (existing && CanUse(*existing, params)) {
    return existing;
  }
  Resource* const resource = bindings::MakeGarbageCollected<Resource>(
      params.type, params.url, params.mode, params.credentials);
  if (params.url.SchemeIs(url::kDataScheme)) {
    // CreateResourceForStaticData(): parsed and loaded now, with no
    // request, and none at all for a URL that does not parse.
    std::optional<DataUrlResponse> data = ParseDataUrl(params.url);
    if (!data) {
      return nullptr;
    }
    resource->ResponseReceived(200, std::move(data->mime_type));
    resource->AppendData(data->body);
    resource->Finish(0);
  } else {
    StartLoad(*resource, params);
  }
  resources_.push_back(resource);
  return resource;
}

void ResourceFetcher::StartLoad(Resource& resource,
                                const FetchParameters& params) {
  Fetcher* const fetcher = document_->DefaultView()->GetFetcher();
  if (!fetcher) {
    // A window with no Session to fetch through.
    resource.Finish(kInvalidUrl);
    return;
  }
  FetchRequestData request;
  request.method = "GET";
  request.url = params.url.spec();
  request.destination = DestinationOf(params.type);
  request.mode = params.mode;
  request.credentials = params.credentials;
  request.referrer_policy = params.referrer_policy;
  const FetchPriority priority = ComputeLoadPriority(params);
  if (priority != TypeToPriority(params.type)) {
    request.priority = priority;
  }
  request.render_blocking = params.render_blocking;
  // Resource::IsLoadEventBlockingResourceType(), for a fetch that is no link
  // preload and that nothing lets go.
  const bool blocks_load = params.load_blocking && !params.is_link_preload;
  if (blocks_load) {
    document_->IncrementLoadEventDelayCount();
  }
  fetcher->Fetch(
      std::move(request),
      std::make_unique<ResourceLoader>(resource, blocks_load ? this : nullptr));
}

void ResourceFetcher::BlockingLoadFinished() {
  if (Document* const document = document_.Get()) {
    document->DecrementLoadEventDelayCount();
  }
}

FetchPriority ResourceFetcher::ComputeLoadPriority(
    const FetchParameters& params) {
  FetchPriority priority = TypeToPriority(params.type);
  // Resources before the first image that the preload scanner finds are
  // early in the document, and those after it late.
  if (params.type == ResourceType::kImage && !params.is_link_preload) {
    image_fetched_ = true;
  }
  if (params.lazy) {
    priority = FetchPriority::kVeryLow;
  }
  priority = AdjustPriorityWithPriorityHintAndRenderBlocking(
      priority, params.priority_hint, params.render_blocking);
  // AdjustImagePriority(): the first five images that the preload scanner
  // finds in the document and that are not small go at kMedium.
  if (params.type == ResourceType::kImage) {
    bool is_small_image = false;
    if (params.resource_width && params.resource_height) {
      is_small_image = *params.resource_width * *params.resource_height <=
                       kSmallImageMaxSize;
    } else if (params.resource_width == 0.0f ||
               params.resource_height == 0.0f) {
      is_small_image = true;
    }
    if (params.speculative_preload_type ==
            SpeculativePreloadType::kInDocument &&
        !params.is_link_preload && boosted_image_count_ < kBoostedImageTarget &&
        !is_small_image) {
      ++boosted_image_count_;
      if (priority < FetchPriority::kMedium &&
          params.priority_hint == FetchPriorityHint::kAuto) {
        priority = FetchPriority::kMedium;
      }
    }
  }
  return priority;
}

}  // namespace mitto
