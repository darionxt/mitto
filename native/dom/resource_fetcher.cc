#include "mitto/dom/resource_fetcher.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/data_url.h"
#include "mitto/dom/document.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_tasks.h"
#include "mitto/encoding/content_type.h"
#include "mitto/header_rules.h"
#include "url/url_constants.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {
namespace {

// net::ERR_INVALID_URL and ERR_ABORTED, which the Page program, with no
// //net, cannot name.
constexpr int kInvalidUrl = -300;
constexpr int kAborted = -3;

// Blink's kSmallImageMaxSize: an image of at most this many square pixels
// by its attributes is small, and gets no boost.
constexpr float kSmallImageMaxSize = 10000;
// kBoostedImageTarget: how many images the preload scanner finds that get a
// boost.
constexpr int kBoostedImageTarget = 5;

// TypeToPriority().
FetchPriority TypeToPriority(ResourceType type) {
  switch (type) {
    case ResourceType::kCSSStyleSheet:
    case ResourceType::kFont:
      return FetchPriority::kVeryHigh;
    case ResourceType::kScript:
    case ResourceType::kRaw:
      return FetchPriority::kHigh;
    case ResourceType::kImage:
      return FetchPriority::kLow;
  }
}

// AdjustPriorityWithPriorityHintAndRenderBlocking().
FetchPriority AdjustPriorityWithPriorityHintAndRenderBlocking(
    FetchPriority priority,
    ResourceType type,
    FetchPriorityHint hint,
    bool render_blocking) {
  switch (hint) {
    case FetchPriorityHint::kAuto:
      break;
    case FetchPriorityHint::kHigh:
      priority = std::max(priority, FetchPriority::kHigh);
      break;
    case FetchPriorityHint::kLow:
      // Early render-blocking CSS goes down to kHigh only.
      if (type == ResourceType::kCSSStyleSheet &&
          priority == FetchPriority::kVeryHigh) {
        priority = FetchPriority::kHigh;
      } else {
        priority = std::min(priority, FetchPriority::kLow);
      }
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
    case ResourceType::kCSSStyleSheet:
      return FetchDestination::kStyle;
    case ResourceType::kScript:
      return FetchDestination::kScript;
    case ResourceType::kFont:
      return FetchDestination::kFont;
    case ResourceType::kRaw:
      return FetchDestination::kEmpty;
  }
}

// The mode and credentials mode of a request of `type`'s Request Kind that
// names none: @font-face's for a font, and fetch()'s for a raw request.
std::pair<FetchMode, FetchCredentialsMode> KindModes(ResourceType type) {
  switch (type) {
    case ResourceType::kImage:
    case ResourceType::kCSSStyleSheet:
    case ResourceType::kScript:
      return {FetchMode::kNoCors, FetchCredentialsMode::kInclude};
    case ResourceType::kFont:
    case ResourceType::kRaw:
      return {FetchMode::kCors, FetchCredentialsMode::kSameOrigin};
  }
}

// Resource::IsLoadEventBlockingResourceType(): a script holds the load event
// back through its element instead.
bool IsLoadEventBlockingResourceType(ResourceType type) {
  switch (type) {
    case ResourceType::kImage:
    case ResourceType::kCSSStyleSheet:
    case ResourceType::kFont:
      return true;
    case ResourceType::kScript:
    case ResourceType::kRaw:
      return false;
  }
}

}  // namespace

// What a resource's fetch hears, which keeps the resource until it ends.
class ResourceFetcher::ResourceLoader final : public FetchClient {
 public:
  ResourceLoader(Resource& resource, ResourceFetcher& fetcher)
      : resource_(&resource), fetcher_(&fetcher) {}

  ResourceLoader(const ResourceLoader&) = delete;
  ResourceLoader& operator=(const ResourceLoader&) = delete;

  ~ResourceLoader() override = default;

  // FetchClient:
  void OnStarted(FetchController& controller) override {
    controller_ = &controller;
  }
  void OnResponseStarted(const FetchResponseHead& head) override {
    std::string mime_type =
        GetMimeTypeAndCharset(HeaderField(head.headers, "content-type"))
            .mime_type;
    resource_->ResponseReceived(head, std::move(mime_type));
  }
  void OnResponseBody(std::string bytes) override {
    resource_->AppendData(bytes);
    // A resource's body is taken as it comes.
    controller_->TookBody(bytes.size());
  }
  void OnResponseEnded(int net_error) override {
    resource_->SetCancelLoad(base::OnceClosure());
    resource_->Finish(net_error);
    if (ResourceFetcher* const fetcher = fetcher_.Get()) {
      fetcher->LoadFinished(*resource_);
    }
  }

  // Ends the fetch, and destroys this before it returns.
  void Cancel() { controller_->Cancel(); }

  base::WeakPtr<ResourceLoader> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

 private:
  const cppgc::Persistent<Resource> resource_;
  const cppgc::WeakPersistent<ResourceFetcher> fetcher_;
  raw_ptr<FetchController> controller_ = nullptr;
  base::WeakPtrFactory<ResourceLoader> weak_factory_{this};
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
  visitor->Trace(blocking_);
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

Resource* ResourceFetcher::UnusedPreload(const GURL& url,
                                         ResourceType type) const {
  const GURL key = WithoutFragment(url);
  for (size_t i = resources_.size(); i > 0; --i) {
    Resource* const resource = resources_[i - 1];
    if (resource->IsUnusedPreload() && resource->GetType() == type &&
        WithoutFragment(resource->Url()) == key) {
      return resource;
    }
  }
  return nullptr;
}

Resource* ResourceFetcher::MatchPreload(const FetchParameters& params) {
  Resource* const resource = UnusedPreload(params.url, params.type);
  if (!resource) {
    return nullptr;
  }
  if (!params.integrity.empty() &&
      !IntegritySetsEqual(resource->Integrity(), params.integrity)) {
    return nullptr;
  }
  if (resource->Mode() != params.mode ||
      resource->Credentials() != params.credentials) {
    return nullptr;
  }
  resource->SetIsUnusedPreload(false);
  if (resource->HttpStatus() == 304) {
    return nullptr;
  }
  return resource;
}

bool ResourceFetcher::CanUse(const Resource& existing,
                             const FetchParameters& params) const {
  // MustRefetchDueToIntegrityMetadata(): a request with integrity metadata
  // takes only a resource fetched with the same.
  if (!params.integrity.empty() &&
      !IntegritySetsEqual(existing.Integrity(), params.integrity)) {
    return false;
  }
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
  if (params.type != ResourceType::kRaw) {
    // During the initial load, one request for a URL, even one that failed.
    const Document* const document = document_.Get();
    if (document && !document->LoadEventFinished()) {
      return true;
    }
    if (existing.IsLoading()) {
      return true;
    }
  }
  if (existing.ErrorOccurred()) {
    return false;
  }
  // Images are reused without cache validation, even with no-store, as
  // kReuseNoStoreImageOnSameSrcReassignment has it.
  if (existing.GetType() == ResourceType::kImage) {
    return true;
  }
  // An unused preload is the one a request matches, as MatchPreload() takes
  // it. What the HTTP cache allows of any other is the Session's: a Known
  // Gap for no-store and validation in the memory cache.
  return existing.IsUnusedPreload();
}

Resource* ResourceFetcher::RequestResource(const FetchParameters& params) {
  Document* const document = document_.Get();
  Window* const window = document ? document->DefaultView() : nullptr;
  if (!window || !params.url.is_valid()) {
    return nullptr;
  }
  // MatchPreload() first: an unused preload of the URL and type, which a
  // newer resource of the URL and another type does not hide, and then the
  // newest resource of the URL, as Blink's memory cache keeps one.
  Resource* existing = UnusedPreload(params.url, params.type);
  if (!existing || !CanUse(*existing, params)) {
    existing = CachedResource(params.url);
  }
  if (existing && CanUse(*existing, params)) {
    if (existing->IsUnusedPreload() && !params.is_link_preload) {
      // A speculative preload takes it and leaves it unused, as
      // MatchPreload() returns it before it marks it.
      if (params.speculative_preload_type ==
          SpeculativePreloadType::kNotSpeculative) {
        existing->SetIsUnusedPreload(false);
      }
      MakePreloadedResourceBlockIfNeeded(*existing, params);
    }
    return existing;
  }
  Resource* const resource = bindings::MakeGarbageCollected<Resource>(
      params.type, params.url, params.mode, params.credentials);
  resource->SetIntegrity(params.integrity);
  resource->SetCharset(params.charset);
  // ResourceFetcher::PrepareRequest(): one that leaves its referrer policy to
  // the document goes with the document's.
  resource->SetReferrerPolicy(params.referrer_policy ==
                                      FetchReferrerPolicy::kDefault
                                  ? document->GetReferrerPolicy()
                                  : params.referrer_policy);
  resource->SetIsUnusedPreload(params.is_link_preload);
  resource->SetFetcher(this);
  resources_.push_back(resource);
  if (params.url.SchemeIs(url::kDataScheme)) {
    // CreateResourceForStaticData(): parsed and loaded now, with no
    // request. One that does not parse fails, as its loader would.
    std::optional<DataUrlResponse> data = ParseDataUrl(params.url);
    if (!data) {
      resource->Finish(kInvalidUrl);
      return resource;
    }
    FetchResponseHead head;
    head.status = 200;
    head.status_text = "OK";
    head.url = params.url.spec();
    head.headers = "Content-Type: " + data->content_type + "\n";
    resource->ResponseReceived(std::move(head), std::move(data->mime_type));
    resource->AppendData(data->body);
    resource->Finish(0);
  } else {
    StartLoad(*resource, params);
  }
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
  // Its modes go only where they are not its kind's own.
  const auto [kind_mode, kind_credentials] = KindModes(params.type);
  if (params.mode != kind_mode) {
    request.mode = params.mode;
  }
  if (params.credentials != kind_credentials) {
    request.credentials = params.credentials;
  }
  request.referrer_policy = resource.ReferrerPolicy();
  const FetchPriority priority = ComputeLoadPriority(params);
  if (priority != TypeToPriority(params.type)) {
    request.priority = priority;
  }
  request.referrer = params.referrer;
  request.render_blocking = params.render_blocking;
  // A fetch of a type that blocks the load event holds it back, unless it is
  // a link preload or nothing lets it go.
  if (IsLoadEventBlockingResourceType(params.type) && params.load_blocking &&
      !params.is_link_preload) {
    blocking_.push_back(&resource);
    document_->IncrementLoadEventDelayCount();
  }
  auto loader = std::make_unique<ResourceLoader>(resource, *this);
  resource.SetCancelLoad(
      base::BindOnce(&ResourceLoader::Cancel, loader->GetWeakPtr()));
  fetcher->Fetch(std::move(request), std::move(loader));
}

void ResourceFetcher::ScheduleCancel(Resource& resource) {
  // ResourceLoader::ScheduleCancel(): a timer of no delay, which cancels the
  // load if still nothing uses it.
  Document* const document = document_.Get();
  Window* const window = document ? document->DefaultView() : nullptr;
  WindowTasks* const tasks = window ? window->GetTasks() : nullptr;
  if (!tasks) {
    return;
  }
  tasks->PostTask(
      TaskType::kInternalLoading,
      base::BindOnce(
          [](const cppgc::WeakPersistent<ResourceFetcher>& fetcher,
             const cppgc::Persistent<Resource>& resource) {
            if (fetcher && resource->IsLoading() && !resource->HasClients()) {
              fetcher->CancelLoad(*resource);
            }
          },
          cppgc::WeakPersistent<ResourceFetcher>(this),
          cppgc::Persistent<Resource>(&resource)));
}

void ResourceFetcher::CancelLoad(Resource& resource) {
  base::OnceClosure cancel = resource.TakeCancelLoad();
  if (!cancel) {
    return;
  }
  // A cancelled resource fails, and goes from the document's resources, as
  // Blink's memory cache lets one go, so the next request fetches again.
  resource.Finish(kAborted);
  LoadFinished(resource);
  resources_.EraseIf([&resource](Resource* item) { return item == &resource; });
  std::move(cancel).Run();
}

void ResourceFetcher::MakePreloadedResourceBlockIfNeeded(
    Resource& resource,
    const FetchParameters& params) {
  // A preload that a request of a load-blocking type matches while it loads
  // holds the load event back from then on.
  if (!resource.IsLoading() || !IsLoadEventBlockingResourceType(params.type)) {
    return;
  }
  blocking_.push_back(&resource);
  if (Document* const document = document_.Get()) {
    document->IncrementLoadEventDelayCount();
  }
}

void ResourceFetcher::LoadFinished(Resource& resource) {
  bool was_blocking = false;
  blocking_.EraseIf([&resource, &was_blocking](Resource* item) {
    if (was_blocking || item != &resource) {
      return false;
    }
    was_blocking = true;
    return true;
  });
  Document* const document = document_.Get();
  if (was_blocking && document) {
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
  const bool late_document_from_preload_scanner =
      params.speculative_preload_type == SpeculativePreloadType::kInDocument &&
      image_fetched_;
  // A preloaded font does not take precedence over critical style sheets or
  // parser-blocking scripts.
  if (params.type == ResourceType::kFont && params.is_link_preload) {
    priority = FetchPriority::kHigh;
  }
  if (params.type == ResourceType::kScript) {
    // A lazy classic script, async, deferred or inserted, is kLow, and a
    // late one that the scanner found kMedium.
    if (params.lazy) {
      priority = FetchPriority::kLow;
    } else if (late_document_from_preload_scanner) {
      priority = FetchPriority::kMedium;
    }
  } else if (params.type == ResourceType::kCSSStyleSheet &&
             late_document_from_preload_scanner) {
    // A late style sheet that the scanner found blocks no rendering.
    priority = FetchPriority::kMedium;
  } else if (params.lazy) {
    priority = FetchPriority::kVeryLow;
  }
  priority = AdjustPriorityWithPriorityHintAndRenderBlocking(
      priority, params.type, params.priority_hint, params.render_blocking);
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
