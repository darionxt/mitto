#include "mitto/page/page_frame.h"

#include <algorithm>
#include <utility>

#include "base/auto_reset.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "mitto/bindings/v8_window.h"
#include "mitto/dom/animation_frame_controller.h"
#include "mitto/dom/cache_storage_host.h"
#include "mitto/dom/document.h"
#include "mitto/dom/document_load_timing.h"
#include "mitto/dom/dom_timer.h"
#include "mitto/dom/error_reporting.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/html_frame_owner_element.h"
#include "mitto/dom/idle_task_controller.h"
#include "mitto/dom/indexed_db_host.h"
#include "mitto/dom/memory_info.h"
#include "mitto/dom/navigation.h"
#include "mitto/dom/page_interfaces.h"
#include "mitto/dom/storage_bucket_host.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_tasks.h"
#include "mitto/dom/xml_document.h"
#include "mitto/page/chrome_object.h"
#include "mitto/page/dedicated_worker_messaging_proxy.h"
#include "mitto/page/document_loader.h"
#include "mitto/page/page_fetcher.h"
#include "mitto/page/page_isolate.h"
#include "mitto/page/page_requests.h"
#include "mitto/page/script_runner.h"
#include "mitto/page/session_cache_storage.h"
#include "mitto/page/session_cookie_jar.h"
#include "mitto/page/session_indexed_db.h"
#include "mitto/page/session_sender.h"
#include "mitto/page/session_storage_buckets.h"
#include "url/origin.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

// RenderFrameImpl's window for a navigation the same as the one that waits,
// which it drops: kNavigationDuplicateDetectionTimeWindow.
constexpr base::TimeDelta kDuplicateWindow = base::Milliseconds(3000);

// A parser's client that gives it no body, for a document that has none, as
// an about:blank that a frame commits synchronously has.
class EmptyBody final : public DocumentParser::Client {
 public:
  explicit EmptyBody(Document& document) : document_(&document) {}

  // DocumentParser::Client:
  bool FeedParser() override {
    if (fed_ || !document_ || !document_->Parser()) {
      return false;
    }
    fed_ = true;
    document_->Parser()->FinishData();
    return true;
  }
  void ParserDetached() override {}

 private:
  cppgc::WeakPersistent<Document> document_;
  bool fed_ = false;
};

}  // namespace

PageFrame::PageFrame(ScriptRunner& runner,
                     uint64_t id,
                     PageFrame* parent,
                     HTMLFrameOwnerElement* owner,
                     std::u16string name)
    : runner_(runner),
      id_(id),
      parent_(parent),
      owner_(owner),
      name_(std::move(name)) {}

PageFrame::~PageFrame() = default;

void PageFrame::Start(const PageConnection& connection) {
  v8::Isolate* const isolate = runner_->isolate();
  const v8::HandleScope handle_scope(isolate);
  // A tab's first document is about:blank of an opaque origin, so its context
  // is no secure context, as in Chrome. A frame's has its parent's document's
  // origin, as Blink's RenderFrameHostImpl::SetOriginDependentStateOfNewFrame()
  // gives it. Its time origin is when its context was made.
  const url::Origin origin =
      parent_ ? parent_->window_->GetDocument().GetOrigin() : url::Origin();
  context_.Reset(isolate, NewContext(base::TimeTicks::Now(), origin));
  window_ = bindings::V8Window::ToWrappable(isolate, context()->Global());
  if (parent_) {
    // The frame's own storage, which asks as each of its documents in turn.
    EventLoop* const event_loop = EventLoop::Get();
    own_cache_storage_ = std::make_unique<SessionCacheStorage>();
    own_cache_storage_->SetSender(
        runner_->sender(), event_loop->TaskRunner(TaskType::kMiscPlatformAPI),
        runner_->refuse());
    own_indexed_db_ = std::make_unique<SessionIndexedDb>();
    own_indexed_db_->SetSender(
        runner_->sender(), event_loop->TaskRunner(TaskType::kDatabaseAccess),
        runner_->refuse());
    own_storage_buckets_ = std::make_unique<SessionStorageBuckets>();
    own_storage_buckets_->SetSender(
        runner_->sender(), event_loop->TaskRunner(TaskType::kMiscPlatformAPI),
        runner_->refuse());
    const v8::Context::Scope context_scope(context());
    Document& document = window_->GetDocument();
    document.SetOrigin(origin);
    // Its about base URL is its creator's base URL, as Blink's
    // DocumentInit::WithFallbackBaseURL() gives it, and its cookie URL its
    // creator's too.
    const Document& creator = parent_->window_->GetDocument();
    document.SetAboutBaseUrl(creator.BaseUrl());
    document.SetCookieUrl(creator.CookieUrl());
    // Its document.cookie and storage are its origin's, for one of an HTTP or
    // HTTPS origin.
    if (origin.GetURL().SchemeIsHTTPOrHTTPS()) {
      document.SetCookieJar(&runner_->cookie_jar());
      GiveStorage(document);
    }
  }
  AttachWindow(connection, PageDocumentSource::kInitialEmpty,
               kNavigationRequest);
}

v8::Local<v8::Context> PageFrame::context() const {
  return context_.Get(runner_->isolate());
}

Window* PageFrame::DomWindow() const {
  return window_.Get();
}

Frame* PageFrame::Parent() const {
  return parent_;
}

size_t PageFrame::ChildCount() const {
  return children_.size();
}

Frame* PageFrame::ChildAt(size_t index) const {
  return children_.at(index).get();
}

HTMLFrameOwnerElement* PageFrame::Owner() const {
  return owner_.Get();
}

const std::u16string& PageFrame::Name() const {
  return name_;
}

void PageFrame::SetName(std::u16string name) {
  name_ = std::move(name);
}

bool PageFrame::IsLoading() const {
  // Frame::IsLoading(): while a navigation waits or the document loads. A
  // frame's initial about:blank, whose load event never fires, holds nothing
  // back, as Blink's FrameLoader::Init() suppresses its finish.
  if (provisional_) {
    return true;
  }
  if (!window_ || !window_->HasDocument()) {
    return false;
  }
  const Document& document = window_->GetDocument();
  return !document.IsInitialEmptyDocument() && !document.LoadEventFinished();
}

Frame* PageFrame::CreateChildFrame(HTMLFrameOwnerElement& owner,
                                   std::u16string name) {
  // Page::MaxNumberOfFrames(): Blink makes no more than 1000 frames for a
  // page.
  constexpr size_t kMaxNumberOfFrames = 1000;
  if (runner_->FrameCount() >= kMaxNumberOfFrames || !window_) {
    return nullptr;
  }
  children_.push_back(std::make_unique<PageFrame>(
      *runner_, runner_->NextFrameId(), this, &owner, std::move(name)));
  PageFrame& child = *children_.back();
  child.Start(window_->Connection());
  return &child;
}

void PageFrame::NavigateFromOwner(const GURL& url) {
  // RenderFrameImpl::BeginNavigation(): the first navigation of a frame on
  // its initial about:blank to about:blank, which its element starts from a
  // document of its origin, commits at once.
  if (!navigated_ && url.IsAboutBlank() &&
      window_->GetDocument().IsInitialEmptyDocument()) {
    CommitAboutBlankSynchronously(url);
    return;
  }
  // A frame's other navigations, to a srcdoc, a data: URL or through the
  // Session, are Known Gaps for now, and it keeps its document.
  navigated_ = true;
}

void PageFrame::Detach() {
  if (!parent_ || going_) {
    return;
  }
  v8::Isolate* const isolate = runner_->isolate();
  const v8::HandleScope handle_scope(isolate);
  // LocalFrame::DetachImpl(): the loading stops, the document unloads, then
  // the frame's children go, each in the same order. An unload listener that
  // removes the element again, or takes an ancestor away, has the frame go
  // then, and this returns once the events have.
  if (!detaching_) {
    detaching_ = true;
    StopAllLoaders();
    if (window_ && window_->HasDocument()) {
      const v8::Context::Scope context_scope(context());
      window_->GetDocument().DispatchUnloadEvents();
    }
    if (going_) {
      return;
    }
  }
  going_ = true;
  DetachChildren();
  const uint64_t document = window_ ? window_->DocumentId() : kNoDocument;
  if (window_) {
    DetachWindow();
  }
  // The Session forgets the frame's document and everything under it, so it
  // hears nothing more of the frame's storage, which outlives the frame for
  // what its scripts made of it.
  runner_->sender()->SendContextGone(document);
  runner_->KeepRemovedFrameStorage(std::move(own_cache_storage_),
                                   std::move(own_indexed_db_),
                                   std::move(own_storage_buckets_));
  if (HTMLFrameOwnerElement* const owner = owner_.Get()) {
    owner->ClearContentFrame();
  }
  PageFrame& parent = *parent_;
  parent.RemoveChild(*this);
  // A frame that still loaded held its parent's load event back, which checks
  // again, as Blink's does once the frame's loading stops. It checks from a
  // task, since the element's removal is still under way, and not while the
  // parent's own document goes.
  if (!parent.detaching_ && !parent.detaching_children_ && parent.window_ &&
      parent.window_->HasDocument()) {
    Document& parent_document = parent.window_->GetDocument();
    if (!parent_document.LoadEventFinished()) {
      parent_document.CheckLoadEventSoon();
    }
  }
}

CacheStorageHost* PageFrame::cache_storage_host() const {
  return parent_ ? own_cache_storage_.get() : runner_->cache_storage_host();
}

IndexedDbHost* PageFrame::indexed_db_host() const {
  return parent_ ? own_indexed_db_.get() : runner_->indexed_db_host();
}

StorageBucketHost* PageFrame::storage_bucket_host() const {
  return parent_ ? own_storage_buckets_.get() : runner_->storage_bucket_host();
}

void PageFrame::GiveStorage(Document& document) const {
  document.SetCacheStorageHost(cache_storage_host());
  document.SetIndexedDbHost(indexed_db_host());
  document.SetStorageBucketHost(storage_bucket_host());
}

void PageFrame::DetachChildren() {
  const base::AutoReset<bool> detaching_children(&detaching_children_, true);
  // Each child takes itself out of `children_` as it goes. One whose children
  // already go, as one of theirs took this frame away, finishes on its own.
  for (;;) {
    const auto child = std::find_if(
        children_.rbegin(), children_.rend(),
        [](const std::unique_ptr<PageFrame>& frame) { return !frame->going_; });
    if (child == children_.rend()) {
      return;
    }
    (*child)->Detach();
  }
}

void PageFrame::RemoveChild(PageFrame& child) {
  const auto found = std::ranges::find_if(
      children_, [&child](const std::unique_ptr<PageFrame>& frame) {
        return frame.get() == &child;
      });
  CHECK(found != children_.end());
  // A script of the child, or of a frame under it, may be what removed it,
  // and still runs, so the child goes once the task ends.
  runner_->KeepRemovedFrame(std::move(*found));
  children_.erase(found);
}

void PageFrame::CommitAboutBlankSynchronously(const GURL& url) {
  navigated_ = true;
  v8::Isolate* const isolate = runner_->isolate();
  const v8::HandleScope handle_scope(isolate);
  const v8::Context::Scope context_scope(context());
  Window& window = *window_;
  Document& old_document = window.GetDocument();
  // FrameLoader::DetachDocument(), for a frame that has committed no
  // navigation yet: the unload events first. An unload listener that removed
  // the frame's element took the frame away, which then commits nothing.
  old_document.DispatchUnloadEvents();
  if (detaching_) {
    return;
  }
  // DocumentLoader::CommitNavigation() with the initial document's origin, in
  // the same window, which Blink keeps for a document of its origin.
  Document* const document = Document::CreateHTML();
  document->SetUrl(base::UTF8ToUTF16(url.spec()));
  const url::Origin origin = old_document.GetOrigin();
  document->SetOrigin(origin);
  if (origin.GetURL().SchemeIsHTTPOrHTTPS()) {
    document->SetCookieJar(&runner_->cookie_jar());
    GiveStorage(*document);
  }
  document->SetLoadTiming(&document_load_timing());
  // It keeps the initial document's about base URL, as Chrome 157 does, not
  // its initiator's base URL of the time, and its cookie URL.
  document->SetAboutBaseUrl(old_document.FallbackBaseUrl());
  document->SetCookieUrl(old_document.CookieUrl());
  // The referrer is the full URL of the document that started it, its
  // parent's, as RenderFrameImpl takes it from the initiator frame.
  if (parent_ && parent_->window_ && parent_->window_->HasDocument()) {
    document->SetReferrer(parent_->window_->GetDocument().UrlForBinding());
  }
  window.SetDocument(document);
  // An empty body, which the parser ends at once, so that DOMContentLoaded,
  // the window's load and the element's load fire before this returns.
  empty_body_ = std::make_unique<EmptyBody>(*document);
  DocumentParser& parser =
      document->OpenForNavigation(*empty_body_, DocumentParser::Kind::kHtml);
  parser.Finish();
}

void PageFrame::ExpectOpeningNavigation() {
  opening_ = std::make_unique<DocumentLoader>(
      runner_->requests(), runner_->send(), *this, kNavigationRequest);
  runner_->requests().SetNavigationClient(*opening_);
}

void PageFrame::BeginNavigation(const NavigationRequest& request) {
  // A frame's navigations through the Session are a Known Gap for now: it
  // keeps its document.
  if (parent_) {
    return;
  }
  const base::TimeTicks now = base::TimeTicks::Now();
  // RenderFrameImpl::BeginNavigationInternal(): a navigation the same as the
  // one that waits, which started less than 3 seconds before while no script
  // wrote a cookie, is dropped, and the one that waits goes on.
  if (provisional_ && request.url == provisional_request_.url &&
      request.reload == provisional_request_.reload &&
      request.user_activation == provisional_request_.user_activation &&
      now - provisional_start_ < kDuplicateWindow &&
      runner_->cookie_jar().cookie_writes() == provisional_cookie_writes_) {
    return;
  }
  // Otherwise the newest wins, as the browser's FrameTreeNode takes the new
  // NavigationRequest, and the one that waited ends with ERR_ABORTED.
  if (provisional_) {
    runner_->requests().Cancel(provisional_->number());
    provisional_.reset();
  }
  provisional_ = std::make_unique<DocumentLoader>(runner_->requests(),
                                                  runner_->send(), *this);
  provisional_request_ = request;
  provisional_start_ = now;
  provisional_cookie_writes_ = runner_->cookie_jar().cookie_writes();
  PageRequest page_request;
  page_request.kind = PageRequestKind::kNavigation;
  page_request.url = request.url.spec();
  page_request.user_activation = request.user_activation;
  page_request.reload = request.reload;
  page_request.referrer_policy = ToPageReferrerPolicy(request.referrer_policy);
  // A worker's thread may number a request in between, so the loader learns
  // its number once the request has it.
  provisional_->SetNumber(runner_->requests().StartNavigation(
      std::move(page_request), *provisional_));
}

void PageFrame::DidNavigateWithinDocument(const GURL& url) {
  const std::string& spec = url.spec();
  // A URL too long for a frame stays the Page's, which no Chrome would have.
  if (spec.size() <= kMaxPayloadSize - 8) {
    runner_->send().Run(
        EncodeSameDocumentNavigation(Window::Current().DocumentId(), spec));
  }
}

bool PageFrame::HasProvisionalNavigation() const {
  return provisional_ != nullptr;
}

void PageFrame::StopAllLoaders() {
  if (current_) {
    current_->StopLoading();
  }
  if (provisional_) {
    runner_->requests().Cancel(provisional_->number());
    provisional_.reset();
  }
}

Document& PageFrame::CommitNavigation(
    DocumentLoader& loader,
    const GURL& url,
    base::TimeTicks navigation_start,
    const PageConnection& connection,
    bool cross_origin_isolated,
    const std::optional<std::u16string>& xml_content_type) {
  std::unique_ptr<DocumentLoader> committing = Take(loader);
  Document& old_document = Window::Current().GetDocument();
  // The old document's entries, which the new one keeps those of its origin
  // of, with how the navigation went.
  Navigation::Carried carried =
      Window::Current().Navigation()->CarryForNextDocument();
  if (committing->number() != kNavigationRequest) {
    carried.navigation_type = provisional_request_.reload    ? u"reload"
                              : provisional_request_.replace ? u"replace"
                                                             : u"push";
  }
  if (old_document.IsInitialEmptyDocument()) {
    // FrameLoader::DetachDocument(), for a frame that has committed no
    // navigation yet: the unload events first, then the loading stops.
    old_document.DispatchUnloadEvents();
    if (current_) {
      current_->StopLoading();
    }
  } else {
    // LocalFrame::DetachImpl(), as RenderDocument swaps in a new frame for
    // each later document: the loading stops first, then the unload events.
    if (current_) {
      current_->StopLoading();
    }
    old_document.DispatchUnloadEvents();
  }
  current_ = std::move(committing);
  const GURL old_url(old_document.UrlForBinding());
  const base::TimeTicks unload_event_start = old_document.UnloadEventStart();
  const base::TimeTicks unload_event_end = old_document.UnloadEventEnd();
  Document& document =
      CommitDocument(current_->number(), url, navigation_start, connection,
                     cross_origin_isolated, xml_content_type);
  // The old document's unload times go to a new one of its origin, as
  // Blink's UnloadEventTimingInfo carries them.
  DocumentLoadTiming& timing = document_load_timing();
  timing.unload_event_start = unload_event_start;
  timing.unload_event_end = unload_event_end;
  timing.has_same_origin_as_previous_document =
      url::IsSameOriginWith(old_url, url);
  timing.is_reload =
      current_->number() != kNavigationRequest && provisional_request_.reload;
  // The isolate's current context is the old document's again here.
  document.DefaultView()->Navigation()->SetCarried(std::move(carried));
  return document;
}

void PageFrame::NavigationEndedWithoutDocument(DocumentLoader& loader) {
  // The loader is on the stack below this, so it goes from a later task.
  base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE,
                                                             Take(loader));
}

DocumentLoadTiming& PageFrame::document_load_timing() {
  return *chrome_object_data_->load_timing;
}

Document& PageFrame::CommitDocument(
    uint64_t navigation,
    const GURL& url,
    base::TimeTicks navigation_start,
    const PageConnection& connection,
    bool cross_origin_isolated,
    const std::optional<std::u16string>& xml_content_type) {
  v8::Isolate* const isolate = runner_->isolate();
  const v8::HandleScope handle_scope(isolate);
  // A script that the Session evaluated in the top-level frame's window and
  // that waits for one of its promises fails, as DevTools' do when a page
  // navigates.
  if (!parent_) {
    runner_->TopLevelContextGoes();
  }
  // The old document's frames go with it, and the Session forgets them with
  // it.
  DetachChildren();
  DetachWindow();
  // What the new document's DOMHighResTimeStamp counts from, as Blink takes
  // it from the navigation's start.
  const base::TimeTicks now = base::TimeTicks::Now();
  const base::TimeTicks time_origin =
      navigation_start.is_null() || navigation_start > now ? now
                                                           : navigation_start;
  // Chrome's site isolation locks a renderer to the site of a top-level
  // document at an HTTP or HTTPS URL.
  if (!parent_) {
    MemoryInfo::SetProcessLockedToSite(url.SchemeIsHTTPOrHTTPS());
  }
  const v8::Local<v8::Context> context = NewContext(
      time_origin, url::Origin::Create(url), cross_origin_isolated);
  context_.Reset(isolate, context);
  window_ = bindings::V8Window::ToWrappable(isolate, context->Global());
  AttachWindow(connection, PageDocumentSource::kNavigationResponse, navigation);
  const v8::Context::Scope context_scope(context);
  Document* const document =
      xml_content_type
          ? static_cast<Document*>(XMLDocument::Create(*xml_content_type))
          : Document::CreateHTML();
  document->SetUrl(base::UTF8ToUTF16(url.spec()));
  // Only a document that the Session loaded over HTTP or HTTPS has cookies
  // there; another's origin is opaque to them.
  if (url.SchemeIsHTTPOrHTTPS()) {
    document->SetCookieJar(&runner_->cookie_jar());
    GiveStorage(*document);
  }
  Window::Current().SetDocument(document);
  return *document;
}

v8::Local<v8::Context> PageFrame::NewContext(base::TimeTicks time_origin,
                                             const url::Origin& origin,
                                             bool cross_origin_isolated) {
  v8::Isolate* const isolate = runner_->isolate();
  ChromeObjectData& chrome_object_data =
      runner_->NewChromeObjectData(time_origin);
  chrome_object_data_ = &chrome_object_data;
  ContextInit init;
  init.time_origin = time_origin;
  // A [SecureContext] member is installed when a context is made.
  init.secure_context = IsOriginPotentiallyTrustworthy(origin);
  init.cross_origin_isolated = cross_origin_isolated;
  PageReporter& reporter = runner_->reporter();
  const v8::Local<v8::Context> context = NewPageContext(
      isolate, init, u"",
      [&reporter, &chrome_object_data](v8::Local<v8::Context> context) {
        // As LocalWindowProxy::Initialize(): V8Initializer's settings of the
        // context, then the inspector, then the embedder.
        InitializePageContext(context);
        reporter.ContextCreated(context);
        InstallChromeObject(context, chrome_object_data);
      });
  // LocalWindowProxy::SetSecurityToken(): contexts of one origin share a
  // token, so that V8 lets each reach the others' objects, and one of an
  // opaque origin keeps V8's own, its global object, which no other context
  // has. Blink gives an initial empty document V8's own too, so that each
  // access asks its CanAccess(), which lets one of the same origin through and
  // tells the browser: a Page has no browser to tell, and nothing yet that
  // asks across origins (#286), so it shares its origin's token.
  // V8 compares tokens as objects, so the token is an internalized string,
  // the same object for the same text, as Blink's V8AtomicString is.
  if (!origin.opaque()) {
    const std::string token = origin.Serialize();
    context->SetSecurityToken(
        v8::String::NewFromUtf8(isolate, token.data(),
                                v8::NewStringType::kInternalized,
                                base::checked_cast<int>(token.size()))
            .ToLocalChecked());
  }
  return context;
}

void PageFrame::AttachWindow(const PageConnection& connection,
                             PageDocumentSource source,
                             uint64_t navigation) {
  v8::Isolate* const isolate = runner_->isolate();
  const v8::Local<v8::Context> context = context_.Get(isolate);
  const v8::Context::Scope context_scope(context);
  // The Session hears of the document before anything that it sends.
  PageDocumentCommitted committed;
  committed.source = source;
  committed.navigation = navigation;
  // Documents and workers share the IDs, which the Session holds in order. A
  // frame's document comes under its parent's current document.
  if (parent_) {
    committed.parent = parent_->window_->DocumentId();
    runner_->sender()->SendContext(committed.parent, [&committed](uint64_t id) {
      committed.document = id;
      return EncodeDocumentCommitted(committed);
    });
  } else {
    runner_->sender()->SendTopLevelDocument([&committed](uint64_t id) {
      committed.document = id;
      return EncodeDocumentCommitted(committed);
    });
  }
  // What the frame's document before asked of the Session's storage goes
  // with it.
  if (CacheStorageHost* const host = cache_storage_host()) {
    host->DocumentCommitted(committed.document);
  }
  if (StorageBucketHost* const host = storage_bucket_host()) {
    host->DocumentCommitted(committed.document);
  }
  if (IndexedDbHost* const host = indexed_db_host()) {
    host->DocumentCommitted(committed.document);
  }
  timers_ = std::make_unique<DOMTimerCoordinator>(isolate, context);
  idle_tasks_ = std::make_unique<IdleTaskController>(isolate, context);
  animation_frames_ =
      std::make_unique<AnimationFrameController>(isolate, context);
  fetcher_ = std::make_unique<PageFetcher>(runner_->requests());
  window_tasks_ = std::make_unique<WindowTasks>(isolate, context);
  // The window's dedicated workers, whose errors that nothing canceled end
  // up at the Session, as the window's own do.
  DedicatedWorkerMessagingProxy::Creator creator(runner_->requests());
  creator.sender = runner_->sender();
  creator.refuse = runner_->refuse();
  creator.report_uncaught = base::BindRepeating(
      [](const SendFrame& send, const ErrorMessageInfo& error) {
        send.Run(EncodeUncaughtError(base::UTF16ToUTF8(error.message)));
      },
      runner_->send());
  worker_starter_ = std::make_unique<PageWorkerStarter>(std::move(creator));
  Window& window = Window::Current();
  window.SetTimers(timers_.get());
  window.SetIdleTasks(idle_tasks_.get());
  window.SetAnimationFrames(animation_frames_.get());
  window.SetFetcher(fetcher_.get());
  window.SetNavigationClient(this);
  window.SetFrame(this);
  window.SetTasks(window_tasks_.get());
  window.SetWorkerStarter(worker_starter_.get());
  if (source == PageDocumentSource::kInitialEmpty) {
    window.GetDocument().SetInitialEmptyDocument();
  }
  window.SetStart(runner_->start(), connection);
  window.SetDocumentId(committed.document);
  window.SetDocumentLoadTiming(chrome_object_data_->load_timing.Get());
  window.SetConsoleSink(base::BindRepeating(
      [](const SendFrame& send, ConsoleLevel level, std::string text) {
        PageConsoleMessage message;
        message.level = level;
        message.text = std::move(text);
        send.Run(EncodeConsole(message));
      },
      runner_->send()));
}

void PageFrame::DetachWindow() {
  v8::Isolate* const isolate = runner_->isolate();
  const v8::Local<v8::Context> context = context_.Get(isolate);
  {
    // The old window's timers and fetches stop with its context.
    const v8::Context::Scope old_scope(context);
    Window::Current().SetTimers(nullptr);
    Window::Current().SetIdleTasks(nullptr);
    Window::Current().SetAnimationFrames(nullptr);
    Window::Current().SetFetcher(nullptr);
    Window::Current().SetNavigationClient(nullptr);
    Window::Current().SetFrame(nullptr);
    Window::Current().SetTasks(nullptr);
    Window::Current().SetWorkerStarter(nullptr);
  }
  // A timer, an idle or animation frame callback, a task or a fetch's client
  // of the window may be what removed its frame, and still run, so what runs
  // them only stops now. It goes with the frame, which the runner keeps until
  // the task ends, or once the next window's takes its place.
  timers_->Stop();
  idle_tasks_->Stop();
  animation_frames_->Stop();
  fetcher_->Stop();
  window_tasks_->Stop();
  runner_->reporter().ContextDestroyed(context);
  chrome_object_data_->detached = true;
}

std::unique_ptr<DocumentLoader> PageFrame::Take(DocumentLoader& loader) {
  if (opening_.get() == &loader) {
    return std::move(opening_);
  }
  CHECK_EQ(provisional_.get(), &loader);
  return std::move(provisional_);
}

}  // namespace mitto
