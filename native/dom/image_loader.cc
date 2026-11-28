#include "mitto/dom/image_loader.h"

#include <cmath>
#include <memory>
#include <tuple>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ref.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/document.h"
#include "mitto/dom/dom_exception.h"
#include "mitto/dom/fetcher.h"
#include "mitto/dom/html_image_element.h"
#include "mitto/dom/html_parser_idioms.h"
#include "mitto/dom/resource_fetcher.h"
#include "mitto/dom/svg_image_element.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_tasks.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-traced-handle.h"

namespace mitto {
namespace {

constexpr char16_t kCannotDecode[] = u"The source image cannot be decoded.";

// ConfigureRequest() and DoUpdateFromElement(): the attributes of the request,
// which Blink reads of any element.
CrossOriginAttribute CrossOriginOf(const Element& element) {
  const std::u16string* value = element.FindAttribute(u"crossorigin");
  return CrossOriginAttributeFromValue(
      value ? std::optional<std::u16string>(*value) : std::nullopt);
}

FetchReferrerPolicy ReferrerPolicyOf(const Element& element) {
  const std::u16string* value = element.FindAttribute(u"referrerpolicy");
  return value ? FetchReferrerPolicyFromStringWithLegacyKeywords(*value)
               : FetchReferrerPolicy::kDefault;
}

FetchPriorityHint FetchPriorityOf(const Element& element) {
  const std::u16string* value = element.FindAttribute(u"fetchpriority");
  return value ? FetchPriorityHintFromAttribute(*value)
               : FetchPriorityHint::kAuto;
}

// A microtask's closure, and the context it runs in.
struct Microtask {
  base::OnceClosure task;
  v8::Global<v8::Context> context;
};

// Runs `task` as a microtask, as Blink's EventLoop::EnqueueMicrotask() does,
// in the context that enqueues it: a checkpoint at the end of a task enters
// none for a microtask of C++.
void EnqueueMicrotask(base::OnceClosure task) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  auto microtask = std::make_unique<Microtask>();
  microtask->task = std::move(task);
  if (isolate->InContext()) {
    microtask->context.Reset(isolate, isolate->GetCurrentContext());
  }
  isolate->EnqueueMicrotask(
      [](void* data) {
        const std::unique_ptr<Microtask> held(static_cast<Microtask*>(data));
        v8::Isolate* const isolate = v8::Isolate::GetCurrent();
        const v8::HandleScope handle_scope(isolate);
        if (held->context.IsEmpty()) {
          std::move(held->task).Run();
          return;
        }
        const v8::Local<v8::Context> context = held->context.Get(isolate);
        const v8::Context::Scope context_scope(context);
        std::move(held->task).Run();
      },
      microtask.release());
}

// The window that the element's document is the active document of, or
// null for a document without one, such as one of createHTMLDocument().
Window* ActiveWindowOf(const Document& document) {
  Window* const window = document.DefaultView();
  if (!window || !window->HasContext() || &window->GetDocument() != &document) {
    return nullptr;
  }
  return window;
}

}  // namespace

// Blink's IncrementLoadEventDelayCount: holds a document's load event back
// while it lives, and lets it go when it goes.
class ImageLoader::LoadEventDelay {
 public:
  explicit LoadEventDelay(Document& document) : document_(&document) {
    document.IncrementLoadEventDelayCount();
  }

  LoadEventDelay(const LoadEventDelay&) = delete;
  LoadEventDelay& operator=(const LoadEventDelay&) = delete;

  ~LoadEventDelay() {
    if (Document* const document = document_.Get()) {
      document->DecrementLoadEventDelayCount();
    }
  }

  // Lets the load event go and checks it at once.
  void ClearAndCheckLoadEvent() {
    if (Document* const document = document_.Get()) {
      document->DecrementLoadEventDelayCountAndCheckLoadEvent();
    }
    document_ = nullptr;
  }

  // The element went to another document, which it holds back instead.
  void DocumentChanged(Document& new_document) {
    new_document.IncrementLoadEventDelayCount();
    if (Document* const document = document_.Get()) {
      document->DecrementLoadEventDelayCount();
    }
    document_ = &new_document;
  }

 private:
  cppgc::WeakPersistent<Document> document_;
};

// A promise of decode(), and how far it is.
class ImageLoader::DecodeRequest final
    : public cppgc::GarbageCollected<DecodeRequest> {
 public:
  enum State {
    kPendingMicrotask,
    kPendingLoad,
    kDispatched,
  };

  DecodeRequest(uint64_t id,
                v8::Isolate* isolate,
                v8::Local<v8::Promise::Resolver> resolver)
      : id_(id) {
    resolver_.Reset(isolate, resolver);
  }

  void Trace(cppgc::Visitor* visitor) const { visitor->Trace(resolver_); }

  uint64_t id() const { return id_; }
  State state() const { return state_; }
  void set_state(State state) { state_ = state; }
  bool settled() const { return resolver_.IsEmpty(); }

  // Resolves or rejects it, in the resolver's context.
  void Settle(bool resolve) {
    if (resolver_.IsEmpty()) {
      return;
    }
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    const v8::HandleScope handle_scope(isolate);
    const v8::Local<v8::Promise::Resolver> resolver = resolver_.Get(isolate);
    resolver_.Reset();
    v8::Local<v8::Context> context;
    if (!resolver->GetCreationContext(isolate).ToLocal(&context)) {
      return;
    }
    const v8::Context::Scope context_scope(context);
    if (resolve) {
      std::ignore = resolver->Resolve(context, v8::Undefined(isolate));
    } else {
      std::ignore = resolver->Reject(
          context,
          DOMException::Create(kCannotDecode, u"EncodingError")->ToV8(isolate));
    }
  }

 private:
  const uint64_t id_;
  State state_ = kPendingMicrotask;
  v8::TracedReference<v8::Promise::Resolver> resolver_;
};

ImageLoader::ImageLoader(HTMLImageElement& element)
    : element_(&element), svg_image_(false) {}

ImageLoader::ImageLoader(SVGImageElement& element)
    : element_(&element), svg_image_(true) {}

ImageLoader::~ImageLoader() = default;

void ImageLoader::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(keep_alive_window_);
  visitor->Trace(element_);
  visitor->Trace(image_content_);
  visitor->Trace(image_content_for_image_document_);
  visitor->Trace(decode_requests_);
}

bool ImageLoader::IsDocumentActive() const {
  return ActiveWindowOf(element_->GetDocument()) != nullptr;
}

bool ImageLoader::HasPendingActivity() const {
  // HasPendingEvent() or a microtask.
  return pending_task_ || pending_load_event_ || pending_error_event_ ||
         !decode_requests_.empty() ||
         (image_content_ && !image_complete_ &&
          lazy_image_load_state_ != LazyImageLoadState::kDeferred);
}

std::optional<std::u16string> ImageLoader::ImageSourceURL() const {
  if (svg_image_) {
    return static_cast<const SVGImageElement&>(*element_).ImageSourceURL();
  }
  return static_cast<const HTMLImageElement&>(*element_).ImageSourceURL();
}

bool ImageLoader::HasLazyLoadingAttribute() const {
  // LazyImageHelper defers an img alone.
  return !svg_image_ && static_cast<const HTMLImageElement&>(*element_)
                            .HasLazyLoadingAttribute();
}

void ImageLoader::DispatchLoadEvent() {
  if (svg_image_) {
    // SVGImageLoader::DispatchLoadEvent(): error for an image that failed,
    // and SVG's load event otherwise.
    if (image_content_->ErrorOccurred()) {
      DispatchErrorEvent();
      return;
    }
    static_cast<SVGImageElement&>(*element_)
        .SendSVGLoadEventToSelfAndAncestorChainIfPossible();
    return;
  }
  static_cast<HTMLImageElement&>(*element_).DispatchLoadEvent();
}

void ImageLoader::DispatchErrorEvent() {
  if (svg_image_) {
    static_cast<SVGImageElement&>(*element_).DispatchErrorEvent();
    return;
  }
  static_cast<HTMLImageElement&>(*element_).DispatchErrorEvent();
}

GURL ImageLoader::ImageSourceToUrl(
    const std::optional<std::u16string>& source) const {
  // No image for an inactive document, or a src that is missing or blank.
  if (!IsDocumentActive() || !source) {
    return GURL();
  }
  const std::u16string_view stripped = StripHTMLSpaces(*source);
  if (stripped.empty()) {
    return GURL();
  }
  return element_->GetDocument().CompleteUrl(stripped);
}

bool ImageLoader::ShouldLoadImmediately(const GURL& url) const {
  if (!url.is_valid()) {
    return false;
  }
  const Document& document = element_->GetDocument();
  const ResourceFetcher* const fetcher = document.ExistingFetcher();
  Resource* const resource = fetcher ? fetcher->CachedResource(url) : nullptr;
  if (!resource || resource->ErrorOccurred()) {
    return false;
  }
  // The list of available images' tuple matching: an image fetched with
  // credentials only for its own origin takes a crossorigin of anonymous.
  const CrossOriginAttribute cross_origin = CrossOriginOf(*element_);
  if (cross_origin != CrossOriginAttribute::kNotSet &&
      !document.GetOrigin().IsSameOriginWith(url)) {
    return false;
  }
  return !(resource->Credentials() == FetchCredentialsMode::kSameOrigin &&
           cross_origin != CrossOriginAttribute::kAnonymous);
}

namespace {

// Whether the document's resource of `url` has loaded without a decode
// error, which a lazy image takes at once, as Blink's UpdateImageState() does
// for one in the memory cache.
bool IsLoadedInDocument(const Document& document, const GURL& url) {
  const ResourceFetcher* const fetcher = document.ExistingFetcher();
  const Resource* const resource =
      fetcher ? fetcher->CachedResource(url) : nullptr;
  return resource && resource->IsLoaded() && !resource->ErrorOccurred();
}

}  // namespace

bool ImageLoader::ImageIsPotentiallyAvailable() const {
  if (lazy_image_load_state_ == LazyImageLoadState::kDeferred) {
    return true;
  }
  const bool image_has_loaded = image_content_ &&
                                !image_content_->IsLoading() &&
                                !image_content_->ErrorOccurred();
  const bool has_pending_activity =
      (image_content_ && !image_complete_) || pending_load_event_ ||
      pending_error_event_ || !decode_requests_.empty() || pending_task_;
  const std::optional<std::u16string> source = ImageSourceURL();
  const bool image_still_loading = !image_has_loaded && has_pending_activity &&
                                   !pending_error_event_ && source &&
                                   !source->empty();
  return (image_has_loaded && image_content_->HasImage()) ||
         image_still_loading;
}

// Updates the keep-alive once the entry point that made it returns.
class ImageLoader::ScopedKeepAliveUpdate {
 public:
  explicit ScopedKeepAliveUpdate(ImageLoader& loader) : loader_(loader) {}
  ScopedKeepAliveUpdate(const ScopedKeepAliveUpdate&) = delete;
  ScopedKeepAliveUpdate& operator=(const ScopedKeepAliveUpdate&) = delete;
  ~ScopedKeepAliveUpdate() { loader_->UpdateKeepAlive(); }

 private:
  const raw_ref<ImageLoader> loader_;
};

void ImageLoader::UpdateKeepAlive() {
  // ActiveScriptWrappableBase::IsContextDestroyed(): nothing keeps an element
  // of a document that is no window's active one with a context, as the old
  // document of a navigation is.
  Window* const window = ActiveWindowOf(element_->GetDocument());
  const bool pending = window && HasPendingActivity();
  if (!pending) {
    keep_alive_.Clear();
    return;
  }
  keep_alive_ = element_.Get();
  if (keep_alive_window_.Get() != window) {
    keep_alive_window_ = window;
    window->AddContextDestroyedCallback(base::BindOnce(
        [](const cppgc::WeakPersistent<ImageLoader>& loader) {
          if (loader) {
            loader->ContextDestroyed();
          }
        },
        cppgc::WeakPersistent<ImageLoader>(this)));
  }
}

void ImageLoader::ContextDestroyed() {
  // A fetch that the navigation cancelled tells the loader nothing more.
  keep_alive_.Clear();
}

void ImageLoader::UpdateFromElement(UpdateFromElementBehavior behavior) {
  const ScopedKeepAliveUpdate keep_alive_update(*this);
  if (!IsDocumentActive()) {
    return;
  }
  const std::optional<std::u16string> source = ImageSourceURL();
  suppress_error_events_ = behavior == kUpdateSizeChanged;
  if (behavior == kUpdateIgnorePreviousError) {
    ClearFailedUrl();
  }
  if (!failed_load_url_.empty() && source == failed_load_url_) {
    return;
  }
  // An image document's img takes the document's own response, as Blink's
  // ImageDocumentParser feeds it, and fetches nothing.
  if (Resource* const content = image_content_for_image_document_.Get()) {
    image_content_for_image_document_ = nullptr;
    SetImageWithoutConsideringPendingLoadEvent(content);
    return;
  }
  // A microtask that waits is dropped: the image loads now, or the microtask
  // starts again.
  if (pending_task_) {
    pending_task_ = 0;
    delay_until_do_update_from_element_.reset();
  }
  const GURL url = ImageSourceToUrl(source);
  if (ShouldLoadImmediately(url) && behavior != kUpdateFromMicrotask) {
    DoUpdateFromElement(behavior, UpdateType::kSync);
    return;
  }
  // img.src = ''; img.src = '...' clears the image down before a load ends.
  if (!source || source->empty()) {
    if (image_content_) {
      image_content_->RemoveClient(this);
    }
    image_content_ = nullptr;
    image_complete_ = true;
    delay_until_image_notify_finished_.reset();
    lazy_image_load_state_ = LazyImageLoadState::kNone;
  } else {
    image_complete_ = false;
  }
  EnqueueImageLoadingMicroTask(behavior);
}

void ImageLoader::EnqueueImageLoadingMicroTask(
    UpdateFromElementBehavior behavior) {
  const uint64_t task = ++last_task_;
  pending_task_ = task;
  EnqueueMicrotask(base::BindOnce(
      [](const cppgc::WeakPersistent<ImageLoader>& loader, uint64_t task,
         UpdateFromElementBehavior behavior) {
        if (loader) {
          loader->RunMicroTask(task, behavior);
        }
      },
      cppgc::WeakPersistent<ImageLoader>(this), task, behavior));
  delay_until_do_update_from_element_ =
      std::make_unique<LoadEventDelay>(element_->GetDocument());
}

void ImageLoader::RunMicroTask(uint64_t task,
                               UpdateFromElementBehavior behavior) {
  const ScopedKeepAliveUpdate keep_alive_update(*this);
  if (task != pending_task_) {
    return;
  }
  DoUpdateFromElement(behavior, UpdateType::kAsync);
}

void ImageLoader::DoUpdateFromElement(UpdateFromElementBehavior behavior,
                                      UpdateType update_type) {
  pending_task_ = 0;
  // Lets the load event go when this returns.
  const std::unique_ptr<LoadEventDelay> load_delay =
      std::move(delay_until_do_update_from_element_);
  Document& document = element_->GetDocument();
  if (!IsDocumentActive()) {
    ClearImage();
    return;
  }
  const std::optional<std::u16string> source = ImageSourceURL();
  const GURL url = ImageSourceToUrl(source);
  Resource* new_image_content = nullptr;
  if (url.is_valid() && !url.is_empty() && behavior != kUpdateForcedReload &&
      lazy_image_load_state_ != LazyImageLoadState::kFullImage &&
      HasLazyLoadingAttribute() && !IsLoadedInDocument(document, url)) {
    // LazyImageHelper::ShouldDeferImageLoad(): a lazy image waits, as Blink
    // defers its fetch, unless the document has it already. It holds no load
    // event back meanwhile.
    Resource* const old_image_content = image_content_.Get();
    if (old_image_content) {
      RejectPendingDecodes(update_type);
      old_image_content->RemoveClient(this);
    }
    CancelPendingLoadEvent();
    if (pending_error_event_) {
      CancelPendingErrorEvent();
    }
    image_content_ = nullptr;
    image_complete_ = false;
    delay_until_image_notify_finished_.reset();
    ClearFailedUrl();
    lazy_image_load_state_ = LazyImageLoadState::kDeferred;
    StartMonitoring();
    return;
  }
  if (url.is_valid() && !url.is_empty()) {
    FetchParameters params;
    params.url = url;
    params.type = ResourceType::kImage;
    switch (CrossOriginOf(*element_)) {
      case CrossOriginAttribute::kNotSet:
        break;
      case CrossOriginAttribute::kAnonymous:
        params.mode = FetchMode::kCors;
        params.credentials = FetchCredentialsMode::kSameOrigin;
        break;
      case CrossOriginAttribute::kUseCredentials:
        params.mode = FetchMode::kCors;
        params.credentials = FetchCredentialsMode::kInclude;
        break;
    }
    params.referrer_policy = ReferrerPolicyOf(*element_);
    params.initiator_name = element_->LocalName();
    params.priority_hint = FetchPriorityOf(*element_);
    // A once-deferred lazy image does not hold the load event back, as
    // FetchParameters::SetLazyImageNonBlocking() lets it go.
    params.load_blocking =
        lazy_image_load_state_ != LazyImageLoadState::kFullImage;
    new_image_content = document.Fetcher().RequestResource(params);
    ClearFailedUrl();
  } else {
    // A src that is no URL, as one of only spaces, fires error; none does
    // not.
    if (source) {
      // One that does not resolve, such as //[, goes to the fetcher as the
      // text that Blink's KURL keeps of it, which blocks it as an access
      // check, so it is the URL that failed, and currentSrc gives the source.
      const std::u16string_view stripped = StripHTMLSpaces(*source);
      if (IsDocumentActive() && !stripped.empty()) {
        const std::string unresolved = document.UnresolvedUrlSpec(stripped);
        if (!unresolved.empty()) {
          failed_load_url_ = base::UTF8ToUTF16(unresolved);
        }
      }
      QueuePendingErrorEvent();
    }
  }
  Resource* const old_image_content = image_content_.Get();
  if (old_image_content != new_image_content) {
    RejectPendingDecodes(update_type);
  }
  CancelPendingLoadEvent();
  // The error of a load that this one cancels goes, but not the error just
  // queued for no image.
  if (pending_error_event_ && new_image_content) {
    CancelPendingErrorEvent();
  }
  UpdateImageState(new_image_content);
  if (new_image_content) {
    // A resource that has ended tells the loader at once.
    new_image_content->AddClient(this);
  }
  if (old_image_content) {
    old_image_content->RemoveClient(this);
  }
}

void ImageLoader::UpdateImageState(Resource* new_image_content) {
  image_content_ = new_image_content;
  image_complete_ = !new_image_content;
  if (!new_image_content) {
    lazy_image_load_state_ = LazyImageLoadState::kNone;
  }
  delay_until_image_notify_finished_.reset();
}

void ImageLoader::DataReceived(Resource& resource) {
  const ScopedKeepAliveUpdate keep_alive_update(*this);
  // ImageLoader::ImageChanged(): from the first data of a load on, the image
  // holds the document's load event back until it notifies its end, as the
  // fetch does before, unless it is one that lazy loading let go.
  if (&resource != image_content_.Get() || image_complete_ ||
      !resource.IsLoading() || delay_until_image_notify_finished_ ||
      !IsDocumentActive()) {
    return;
  }
  delay_until_image_notify_finished_ =
      std::make_unique<LoadEventDelay>(element_->GetDocument());
}

void ImageLoader::SetImageWithoutConsideringPendingLoadEvent(
    Resource* new_image_content) {
  Resource* const old_image_content = image_content_.Get();
  if (new_image_content == old_image_content) {
    return;
  }
  CancelPendingLoadEvent();
  CancelPendingErrorEvent();
  UpdateImageState(new_image_content);
  if (new_image_content) {
    new_image_content->AddClient(this);
  }
  if (old_image_content) {
    old_image_content->RemoveClient(this);
  }
}

void ImageLoader::ClearImage() {
  SetImageWithoutConsideringPendingLoadEvent(nullptr);
}

void ImageLoader::NotifyFinished(Resource& resource) {
  const ScopedKeepAliveUpdate keep_alive_update(*this);
  if (&resource != image_content_.Get() || image_complete_) {
    return;
  }
  image_complete_ = true;
  DispatchDecodeRequestsIfComplete();
  if (resource.ErrorOccurred()) {
    CancelPendingLoadEvent();
    // A CORS check that fails is an access check, and so is what blocks a
    // request: the same URL does not load again until src changes. What
    // blocks a redirect is one too.
    if (resource.NetError() == kBlockedByClient && !resource.IsBlocked()) {
      redirect_blocked_ = true;
    } else if (resource.LoadFailed() &&
               (resource.Mode() == FetchMode::kCors || resource.IsBlocked())) {
      failed_load_url_ = base::UTF8ToUTF16(resource.Url().spec());
    }
    QueuePendingErrorEvent();
  } else {
    QueuePendingLoadEvent();
  }
  delay_until_image_notify_finished_.reset();
}

void ImageLoader::QueuePendingErrorEvent() {
  // Not for a change of the environment.
  if (suppress_error_events_) {
    return;
  }
  Window* const window = ActiveWindowOf(element_->GetDocument());
  WindowTasks* const tasks = window ? window->GetTasks() : nullptr;
  if (!tasks) {
    return;
  }
  // One that waits already gives way to this one.
  CancelPendingErrorEvent();
  const uint64_t event = ++last_event_;
  pending_error_event_ = event;
  delay_until_error_event_ =
      std::make_unique<LoadEventDelay>(element_->GetDocument());
  tasks->PostTask(
      TaskType::kDOMManipulation,
      base::BindOnce(
          [](const cppgc::WeakPersistent<ImageLoader>& loader, uint64_t event) {
            if (loader) {
              loader->DispatchPendingEvent(event, /*is_load=*/false);
            }
          },
          cppgc::WeakPersistent<ImageLoader>(this), event));
}

void ImageLoader::QueuePendingLoadEvent() {
  Window* const window = ActiveWindowOf(element_->GetDocument());
  WindowTasks* const tasks = window ? window->GetTasks() : nullptr;
  if (!tasks) {
    return;
  }
  const uint64_t event = ++last_event_;
  pending_load_event_ = event;
  delay_until_load_event_ =
      std::make_unique<LoadEventDelay>(element_->GetDocument());
  tasks->PostTask(
      TaskType::kDOMManipulation,
      base::BindOnce(
          [](const cppgc::WeakPersistent<ImageLoader>& loader, uint64_t event) {
            if (loader) {
              loader->DispatchPendingEvent(event, /*is_load=*/true);
            }
          },
          cppgc::WeakPersistent<ImageLoader>(this), event));
}

void ImageLoader::CancelPendingLoadEvent() {
  pending_load_event_ = 0;
  delay_until_load_event_.reset();
}

void ImageLoader::CancelPendingErrorEvent() {
  pending_error_event_ = 0;
  delay_until_error_event_.reset();
}

void ImageLoader::DispatchPendingEvent(uint64_t event, bool is_load) {
  const ScopedKeepAliveUpdate keep_alive_update(*this);
  uint64_t& pending = is_load ? pending_load_event_ : pending_error_event_;
  if (!event || event != pending) {
    return;
  }
  pending = 0;
  std::unique_ptr<LoadEventDelay> delay =
      std::move(is_load ? delay_until_load_event_ : delay_until_error_event_);
  if (is_load) {
    if (!image_content_) {
      return;
    }
    DispatchLoadEvent();
  } else {
    DispatchErrorEvent();
  }
  // The document's load event is checked at once.
  if (delay) {
    delay->ClearAndCheckLoadEvent();
  }
}

void ImageLoader::StartMonitoring() {
  Window* const window = ActiveWindowOf(element_->GetDocument());
  if (!window) {
    return;
  }
  const uint64_t monitoring = ++monitoring_;
  window->RequestPostLifecycleSteps(base::BindOnce(
      [](const cppgc::WeakPersistent<ImageLoader>& loader,
         uint64_t monitoring) {
        // One out of the document has no box to come near the viewport, and
        // waits until it is inserted.
        if (loader && loader->monitoring_ == monitoring &&
            loader->element_->IsConnected()) {
          loader->LoadDeferredImage();
        }
      },
      cppgc::WeakPersistent<ImageLoader>(this), monitoring));
}

void ImageLoader::ElementInserted() {
  const ScopedKeepAliveUpdate keep_alive_update(*this);
  if (lazy_image_load_state_ == LazyImageLoadState::kDeferred &&
      element_->IsConnected()) {
    StartMonitoring();
  }
}

void ImageLoader::LoadDeferredImage() {
  const ScopedKeepAliveUpdate keep_alive_update(*this);
  if (lazy_image_load_state_ != LazyImageLoadState::kDeferred) {
    return;
  }
  lazy_image_load_state_ = LazyImageLoadState::kFullImage;
  UpdateFromElement(kUpdateNormal);
}

void ImageLoader::ElementDidMoveToNewDocument() {
  const ScopedKeepAliveUpdate keep_alive_update(*this);
  Document& document = element_->GetDocument();
  if (delay_until_do_update_from_element_) {
    delay_until_do_update_from_element_->DocumentChanged(document);
  }
  if (delay_until_image_notify_finished_) {
    delay_until_image_notify_finished_->DocumentChanged(document);
  }
  ClearFailedUrl();
  ClearImage();
}

void ImageLoader::DensityCorrectedNaturalSize(float inverse_density,
                                              uint32_t& width,
                                              uint32_t& height) const {
  width = 0;
  height = 0;
  if (!image_content_ || !image_content_->HasImage() ||
      image_content_->ErrorOccurred()) {
    return;
  }
  const ImageHeader& image = *image_content_->Image();
  if (image.kind == ImageHeader::Kind::kSvg) {
    // Only natural dimensions scale; the default object size does not.
    NaturalSizingInfo sizing = image.sizing;
    sizing.width *= inverse_density;
    sizing.height *= inverse_density;
    float concrete_width;
    float concrete_height;
    ConcreteObjectSize(sizing, 300, 150, concrete_width, concrete_height);
    // PhysicalSize::FromSizeFFloor(), then rounded to integers.
    width = static_cast<uint32_t>(
        std::max(0.0f, std::floor(concrete_width * 64) / 64 + 0.5f));
    height = static_cast<uint32_t>(
        std::max(0.0f, std::floor(concrete_height * 64) / 64 + 0.5f));
    return;
  }
  // PhysicalSize of the scaled size, as LayoutUnit truncates it to 1/64.
  width = static_cast<uint32_t>(
      std::floor(std::floor(image.width * inverse_density * 64) / 64));
  height = static_cast<uint32_t>(
      std::floor(std::floor(image.height * inverse_density * 64) / 64));
}

v8::Local<v8::Promise> ImageLoader::Decode(
    bindings::ExceptionState& exception_state) {
  const ScopedKeepAliveUpdate keep_alive_update(*this);
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return v8::Local<v8::Promise>();
  }
  // A document that has gone, as an img of a frame that went, rejects at
  // once.
  if (!element_->GetDocument().DefaultView()) {
    exception_state.ThrowDOMException("EncodingError",
                                      "The source image cannot be decoded.");
    return v8::Local<v8::Promise>();
  }
  const uint64_t id = ++last_decode_request_;
  decode_requests_.push_back(
      bindings::MakeGarbageCollected<DecodeRequest>(id, isolate, resolver));
  EnqueueMicrotask(base::BindOnce(
      [](const cppgc::WeakPersistent<ImageLoader>& loader, uint64_t id) {
        if (loader) {
          loader->ProcessDecodeRequest(id);
        }
      },
      cppgc::WeakPersistent<ImageLoader>(this), id));
  return resolver->GetPromise();
}

void ImageLoader::ProcessDecodeRequest(uint64_t id) {
  const ScopedKeepAliveUpdate keep_alive_update(*this);
  for (size_t i = 0; i < decode_requests_.size(); ++i) {
    DecodeRequest* const request = decode_requests_[i];
    if (request->id() == id) {
      if (request->state() != DecodeRequest::kPendingMicrotask) {
        return;
      }
      request->set_state(DecodeRequest::kPendingLoad);
      DispatchDecodeRequestsIfComplete();
      return;
    }
  }
}

void ImageLoader::DispatchDecodeRequestsIfComplete() {
  if (!image_complete_) {
    return;
  }
  if (!IsDocumentActive() || !image_content_ ||
      image_content_->ErrorOccurred()) {
    RejectPendingDecodes();
    return;
  }
  // The Page decodes no pixels: a request that waits for the load resolves,
  // from a task, as the compositor's decode answers it later. An SVG image
  // needs no decode and resolves at once.
  const bool is_svg = image_content_->HasImage() &&
                      image_content_->Image()->kind == ImageHeader::Kind::kSvg;
  Window* const window = ActiveWindowOf(element_->GetDocument());
  WindowTasks* const tasks = window ? window->GetTasks() : nullptr;
  std::vector<DecodeRequest*> to_resolve;
  for (size_t i = 0; i < decode_requests_.size(); ++i) {
    DecodeRequest* const request = decode_requests_[i];
    if (request->state() == DecodeRequest::kPendingLoad) {
      request->set_state(DecodeRequest::kDispatched);
      to_resolve.push_back(request);
    }
  }
  for (DecodeRequest* request : to_resolve) {
    if (is_svg || !tasks) {
      DecodeRequestFinished(request->id());
      continue;
    }
    // It stays among the requests until the answer comes, so that a source
    // change before then rejects it.
    tasks->PostTask(
        TaskType::kInternalDefault,
        base::BindOnce(
            [](const cppgc::WeakPersistent<ImageLoader>& loader, uint64_t id) {
              if (loader) {
                loader->DecodeRequestFinished(id);
              }
            },
            cppgc::WeakPersistent<ImageLoader>(this), request->id()));
  }
}

void ImageLoader::DecodeRequestFinished(uint64_t id) {
  const ScopedKeepAliveUpdate keep_alive_update(*this);
  for (size_t i = 0; i < decode_requests_.size(); ++i) {
    DecodeRequest* const request = decode_requests_[i];
    if (request->id() != id) {
      continue;
    }
    const cppgc::Persistent<DecodeRequest> settled(request);
    decode_requests_.EraseIf(
        [id](DecodeRequest* item) { return item->id() == id; });
    settled->Settle(/*resolve=*/true);
    return;
  }
}

void ImageLoader::RejectPendingDecodes(UpdateType update_type) {
  // A request whose microtask has not run waits for it, as it may act on the
  // next image, unless the update is synchronous, which conceptually came
  // after it.
  std::vector<DecodeRequest*> rejected;
  for (size_t i = 0; i < decode_requests_.size(); ++i) {
    DecodeRequest* const request = decode_requests_[i];
    if (update_type == UpdateType::kAsync &&
        request->state() == DecodeRequest::kPendingMicrotask) {
      continue;
    }
    rejected.push_back(request);
  }
  decode_requests_.EraseIf([update_type](DecodeRequest* request) {
    return !(update_type == UpdateType::kAsync &&
             request->state() == DecodeRequest::kPendingMicrotask);
  });
  for (DecodeRequest* request : rejected) {
    request->Settle(/*resolve=*/false);
  }
}

}  // namespace mitto
