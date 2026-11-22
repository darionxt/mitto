#include "mitto/page/page_frame.h"

#include <utility>

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
#include "mitto/dom/frame_loader.h"
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

// net::OK and net::ERR_ABORTED, the net errors of a navigation that ended
// with no document and no failure.
constexpr int kNetOk = 0;
constexpr int kNetErrorAborted = -3;

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

// A parser's client that gives it `text`, then its end, as a srcdoc document's
// parser reads the attribute's value that the navigation took.
class TextBody final : public DocumentParser::Client {
 public:
  TextBody(Document& document, std::u16string text)
      : document_(&document), text_(std::move(text)) {}

  // DocumentParser::Client:
  bool FeedParser() override {
    DocumentParser* const parser = document_ ? document_->Parser() : nullptr;
    if (!parser || ended_) {
      return false;
    }
    if (!fed_) {
      fed_ = true;
      parser->AppendData(text_);
      return true;
    }
    ended_ = true;
    parser->FinishData();
    return true;
  }
  void ParserDetached() override {}

 private:
  cppgc::WeakPersistent<Document> document_;
  const std::u16string text_;
  bool fed_ = false;
  bool ended_ = false;
};

}  // namespace

PageFrame::LocalNavigation::LocalNavigation() = default;
PageFrame::LocalNavigation::LocalNavigation(LocalNavigation&&) = default;
PageFrame::LocalNavigation& PageFrame::LocalNavigation::operator=(
    LocalNavigation&&) = default;
PageFrame::LocalNavigation::~LocalNavigation() = default;

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
    document.SetFallbackBaseUrl(parent_->window_->GetDocument().BaseUrl());
    // Its document.cookie and storage are its origin's, for one of an HTTP or
    // HTTPS origin.
    if (origin.GetURL().SchemeIsHTTPOrHTTPS()) {
      document.SetCookieJar(&runner_->cookie_jar());
      GiveStorage(document);
    }
  }
  AttachWindow(connection, PageDocumentCommitted());
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
  if (provisional_ || pending_local_navigation_ != 0) {
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
  navigated_ = true;
  // FrameLoader::StartNavigation() of the frame, from its parent's window, as
  // HTMLFrameOwnerElement::LoadOrRedirectSubframe() navigates a frame that it
  // has: within the document, a javascript: URL, or BeginNavigation().
  v8::Isolate* const isolate = runner_->isolate();
  const v8::HandleScope handle_scope(isolate);
  const v8::Context::Scope context_scope(context());
  FrameLoadRequest request;
  request.url = url;
  request.origin_window = parent_->window_.Get();
  Navigate(*window_, request, FrameLoadType::kStandard);
}

void PageFrame::CommitWithoutNetworkSoon(LocalNavigation navigation) {
  navigation.number = ++last_local_navigation_;
  pending_local_navigation_ = navigation.number;
  EventLoop::Get()
      ->TaskRunner(TaskType::kInternalLoading)
      ->PostTask(FROM_HERE, base::BindOnce(&PageFrame::CommitWithoutNetwork,
                                           weak_factory_.GetWeakPtr(),
                                           std::move(navigation)));
}

void PageFrame::CommitWithoutNetwork(LocalNavigation navigation) {
  if (pending_local_navigation_ != navigation.number || !window_) {
    return;
  }
  pending_local_navigation_ = 0;
  v8::Isolate* const isolate = runner_->isolate();
  const v8::HandleScope handle_scope(isolate);
  const GURL& url = navigation.url;
  PageDocumentCommitted& committed = navigation.committed;
  const url::Origin& origin = navigation.origin;
  Document& old_document = window_->GetDocument();
  committed.replaces = window_->DocumentId();
  // Blink keeps the window of a frame's initial about:blank for a document of
  // its origin, as DocumentLoader::CommitNavigation() reuses it, and makes a
  // new one for any other.
  const bool reuse_window = old_document.IsInitialEmptyDocument() &&
                            old_document.GetOrigin().IsSameOriginWith(origin);
  {
    const v8::Context::Scope old_scope(context());
    if (current_) {
      current_->StopLoading();
    }
    old_document.DispatchUnloadEvents();
  }
  DetachChildren();
  Document* document = nullptr;
  if (reuse_window) {
    const v8::Context::Scope context_scope(context());
    window_->SetDocumentId(SendDocumentCommitted(committed));
    document = Document::CreateHTML();
    document_load_timing().Restart(base::TimeTicks::Now());
    document->SetLoadTiming(&document_load_timing());
    window_->SetDocument(document);
  } else {
    // The frame keeps its WindowProxy, which takes the new window.
    const v8::Local<v8::Object> global_proxy = context()->Global();
    DetachWindow();
    context()->DetachGlobal();
    const v8::Local<v8::Context> context =
        NewContext(base::TimeTicks::Now(), origin,
                   /*cross_origin_isolated=*/false, global_proxy);
    context_.Reset(isolate, context);
    window_ = bindings::V8Window::ToWrappable(isolate, context->Global());
    AttachWindow(parent_->window_->Connection(), committed);
    const v8::Context::Scope context_scope(context);
    document = Document::CreateHTML();
    window_->SetDocument(document);
  }
  current_.reset();
  const v8::Context::Scope context_scope(context());
  document->SetUrl(base::UTF8ToUTF16(url.spec()));
  document->SetOrigin(origin);
  document->SetReferrer(navigation.referrer);
  document->SetFallbackBaseUrl(navigation.fallback_base_url);
  if (origin.GetURL().SchemeIsHTTPOrHTTPS()) {
    document->SetCookieJar(&runner_->cookie_jar());
    GiveStorage(*document);
  }
  // The body, the srcdoc or nothing, goes to the parser as a response's would.
  local_body_ =
      std::make_unique<TextBody>(*document, std::move(navigation.body));
  DocumentParser& parser =
      document->OpenForNavigation(*local_body_, DocumentParser::Kind::kHtml);
  parser.DidReceiveData();
  if (document->Parser()) {
    document->Parser()->Finish();
  }
}

void PageFrame::Detach() {
  // An unload listener that removes the element again finds the frame going.
  if (!parent_ || detaching_) {
    return;
  }
  detaching_ = true;
  v8::Isolate* const isolate = runner_->isolate();
  const v8::HandleScope handle_scope(isolate);
  // LocalFrame::DetachImpl(): the loading stops, the document unloads, then
  // the frame's children go, each in the same order.
  StopAllLoaders();
  if (window_ && window_->HasDocument()) {
    const v8::Context::Scope context_scope(context());
    window_->GetDocument().DispatchUnloadEvents();
  }
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
  // This goes now.
  parent_->RemoveChild(*this);
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
  // Each child takes itself out of `children_` as it goes.
  while (!children_.empty()) {
    PageFrame& child = *children_.back();
    child.StopAllLoaders();
    {
      v8::Isolate* const isolate = runner_->isolate();
      const v8::HandleScope handle_scope(isolate);
      const v8::Context::Scope context_scope(child.context());
      child.window_->GetDocument().DispatchUnloadEvents();
    }
    child.DetachChildren();
    child.DetachWindow();
    if (HTMLFrameOwnerElement* const owner = child.owner_.Get()) {
      owner->ClearContentFrame();
    }
    children_.pop_back();
  }
}

void PageFrame::RemoveChild(PageFrame& child) {
  std::erase_if(children_, [&child](const std::unique_ptr<PageFrame>& frame) {
    return frame.get() == &child;
  });
}

void PageFrame::CommitAboutBlankSynchronously(const GURL& url) {
  navigated_ = true;
  v8::Isolate* const isolate = runner_->isolate();
  const v8::HandleScope handle_scope(isolate);
  const v8::Context::Scope context_scope(context());
  Window& window = *window_;
  Document& old_document = window.GetDocument();
  // FrameLoader::DetachDocument(), for a frame that has committed no
  // navigation yet: the unload events first.
  old_document.DispatchUnloadEvents();
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
  document_load_timing().Restart(base::TimeTicks::Now());
  document->SetLoadTiming(&document_load_timing());
  // The referrer is the full URL of the document that started it, its
  // parent's, as RenderFrameImpl takes it from the initiator frame.
  if (parent_ && parent_->window_ && parent_->window_->HasDocument()) {
    Document& parent_document = parent_->window_->GetDocument();
    document->SetReferrer(parent_document.UrlForBinding());
    document->SetFallbackBaseUrl(parent_document.BaseUrl());
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
  if (parent_ && (request.url.IsAboutSrcdoc() || request.url.IsAboutBlank())) {
    // A srcdoc or an about:blank goes through Chrome's browser and commits
    // from a later task, with the srcdoc that an iframe has now. The newest
    // navigation takes the place of one that waits.
    LocalNavigation navigation;
    navigation.url = request.url;
    if (request.url.IsAboutSrcdoc()) {
      navigation.committed.source = PageDocumentSource::kSrcdoc;
      // Only an iframe's srcdoc reaches its document, as Chrome's browser
      // takes it.
      HTMLFrameOwnerElement* const owner = owner_.Get();
      const std::u16string* const srcdoc =
          owner && owner->HasHTMLTagName(u"iframe")
              ? owner->FindAttribute(u"srcdoc")
              : nullptr;
      navigation.body = srcdoc ? *srcdoc : std::u16string();
      Document& parent_document = parent_->window_->GetDocument();
      navigation.origin = parent_document.GetOrigin();
      navigation.referrer = parent_document.UrlForBinding();
      navigation.fallback_base_url = parent_document.BaseUrl();
    } else {
      navigation.committed.source = PageDocumentSource::kAboutBlank;
      navigation.committed.initiator = request.initiator;
      navigation.origin = request.initiator_origin;
      navigation.referrer = base::UTF8ToUTF16(request.initiator_url.spec());
      navigation.fallback_base_url = request.initiator_base_url;
    }
    StopAllLoaders();
    CommitWithoutNetworkSoon(std::move(navigation));
    return;
  }
  // A frame's navigation to a data: or blob: URL is a Known Gap for now: the
  // frame keeps its document.
  if (parent_ && !request.url.SchemeIsHTTPOrHTTPS()) {
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
  // So does a srcdoc or an about:blank that waits.
  pending_local_navigation_ = 0;
  provisional_ = std::make_unique<DocumentLoader>(runner_->requests(),
                                                  runner_->send(), *this);
  provisional_request_ = request;
  provisional_start_ = now;
  provisional_cookie_writes_ = runner_->cookie_jar().cookie_writes();
  PageRequest page_request;
  // A frame's navigation names the frame's document that the new one takes
  // the place of, which the Session counts the frame tree from. A frame
  // element's goes as an iframe's too until the Session has Baselines of
  // destination frame.
  if (parent_) {
    page_request.kind = PageRequestKind::kIframe;
    page_request.target = window_->DocumentId();
  } else {
    page_request.kind = PageRequestKind::kNavigation;
  }
  page_request.url = request.url.spec();
  page_request.user_activation = request.user_activation;
  page_request.reload = request.reload;
  page_request.referrer_policy = ToPageReferrerPolicy(request.referrer_policy);
  // A worker's thread may number a request in between, so the loader learns
  // its number once the request has it.
  provisional_->SetNumber(runner_->requests().StartNavigation(
      request.initiator, std::move(page_request), *provisional_));
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

void PageFrame::NavigationEndedWithoutDocument(DocumentLoader& loader,
                                               int net_error) {
  // The loader is on the stack below this, so it goes from a later task.
  base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE,
                                                             Take(loader));
  // NavigationRequest::OnRequestFailedInternal(): a child frame's navigation
  // that fails commits Chrome's error page, whose load its parent waits for.
  // One that ends with no document, a 204, a 205 or a download, or that
  // another took the place of, ends with ERR_ABORTED, and the frame keeps
  // its document.
  if (parent_ && net_error != kNetErrorAborted && net_error != kNetOk) {
    LocalNavigation navigation;
    navigation.url = GURL(kErrorPageUrl);
    navigation.committed.source = PageDocumentSource::kErrorPage;
    CommitWithoutNetworkSoon(std::move(navigation));
    return;
  }
  // FrameLoader::DidFinishNavigation(): a frame that no longer loads has its
  // parent check whether all of its frames have loaded.
  if (parent_ && !IsLoading() && parent_->window_ &&
      parent_->window_->HasDocument()) {
    const v8::Context::Scope context_scope(parent_->context());
    parent_->window_->GetDocument().CheckCompleted();
  }
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
  // A frame's document takes the place of the frame's in the Session.
  PageDocumentCommitted committed;
  committed.source = PageDocumentSource::kNavigationResponse;
  committed.navigation = navigation;
  if (parent_) {
    committed.replaces = window_->DocumentId();
  }
  const url::Origin origin = url::Origin::Create(url);
  // Blink keeps the window of a frame's initial about:blank for a document of
  // its origin, as DocumentLoader::CommitNavigation() reuses it. A top-level
  // frame's initial about:blank has an opaque origin, so it never does.
  const Document& old_document = window_->GetDocument();
  const bool reuse_window = old_document.IsInitialEmptyDocument() &&
                            old_document.GetOrigin().IsSameOriginWith(origin);
  // The old document's frames go with it, and the Session forgets them with
  // it.
  DetachChildren();
  if (reuse_window) {
    const v8::Context::Scope context_scope(context());
    window_->SetDocumentId(SendDocumentCommitted(committed));
  } else {
    // The frame keeps its WindowProxy, which takes the new window, as
    // LocalWindowProxy detaches the old global object from it.
    const v8::Local<v8::Object> global_proxy = context()->Global();
    DetachWindow();
    context()->DetachGlobal();
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
    const v8::Local<v8::Context> context =
        NewContext(time_origin, origin, cross_origin_isolated, global_proxy);
    context_.Reset(isolate, context);
    window_ = bindings::V8Window::ToWrappable(isolate, context->Global());
    AttachWindow(connection, committed);
  }
  const v8::Context::Scope context_scope(context());
  Document* const document =
      xml_content_type
          ? static_cast<Document*>(XMLDocument::Create(*xml_content_type))
          : Document::CreateHTML();
  if (reuse_window) {
    // The window keeps its time origin, and the document's times start
    // again at the navigation's start.
    const base::TimeTicks now = base::TimeTicks::Now();
    document_load_timing().Restart(navigation_start.is_null() ||
                                           navigation_start > now
                                       ? now
                                       : navigation_start);
    document->SetLoadTiming(&document_load_timing());
  }
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

v8::Local<v8::Context> PageFrame::NewContext(
    base::TimeTicks time_origin,
    const url::Origin& origin,
    bool cross_origin_isolated,
    v8::Local<v8::Object> global_proxy) {
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
      isolate, init, u"", global_proxy,
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

uint64_t PageFrame::SendDocumentCommitted(PageDocumentCommitted committed) {
  // Documents and workers share the IDs, which the Session holds in order. A
  // frame's document comes under its parent's current document, in place of
  // the frame's document that it replaces, if any.
  const auto encode = [&committed](uint64_t id) {
    committed.document = id;
    return EncodeDocumentCommitted(committed);
  };
  if (!parent_) {
    runner_->sender()->SendTopLevelDocument(encode);
  } else {
    committed.parent = parent_->window_->DocumentId();
    if (committed.replaces != kNoDocument) {
      runner_->sender()->SendFrameDocument(committed.parent, committed.replaces,
                                           encode);
    } else {
      runner_->sender()->SendContext(committed.parent, encode);
    }
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
  return committed.document;
}

void PageFrame::AttachWindow(const PageConnection& connection,
                             const PageDocumentCommitted& committed_as) {
  v8::Isolate* const isolate = runner_->isolate();
  const v8::Local<v8::Context> context = context_.Get(isolate);
  const v8::Context::Scope context_scope(context);
  // The Session hears of the document before anything that it sends.
  const uint64_t document = SendDocumentCommitted(committed_as);
  const PageDocumentSource source = committed_as.source;
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
  window.SetDocumentId(document);
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
  }
  timers_.reset();
  idle_tasks_.reset();
  animation_frames_.reset();
  fetcher_.reset();
  window_tasks_.reset();
  worker_starter_.reset();
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
