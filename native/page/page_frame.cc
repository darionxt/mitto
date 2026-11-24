#include "mitto/page/page_frame.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "base/auto_reset.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/scoped_refptr.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/timer/timer.h"
#include "mitto/bindings/v8_window.h"
#include "mitto/dom/animation_frame_controller.h"
#include "mitto/dom/cache_storage_host.h"
#include "mitto/dom/cookie_store.h"
#include "mitto/dom/document.h"
#include "mitto/dom/document_load_timing.h"
#include "mitto/dom/dom_timer.h"
#include "mitto/dom/error_reporting.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/frame_loader.h"
#include "mitto/dom/history_item.h"
#include "mitto/dom/html_frame_owner_element.h"
#include "mitto/dom/idle_task_controller.h"
#include "mitto/dom/indexed_db_host.h"
#include "mitto/dom/memory_info.h"
#include "mitto/dom/navigation.h"
#include "mitto/dom/page_interfaces.h"
#include "mitto/dom/page_reveal_event.h"
#include "mitto/dom/storage_bucket_host.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_task.h"
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
#include "mitto/page/session_history.h"
#include "mitto/page/session_indexed_db.h"
#include "mitto/page/session_sender.h"
#include "mitto/page/session_storage_buckets.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "url/url_constants.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

// RenderFrameImpl's window for a navigation the same as the one that waits,
// which it drops: kNavigationDuplicateDetectionTimeWindow.
constexpr base::TimeDelta kDuplicateWindow = base::Milliseconds(3000);

// What a traversal request's payload holds beside its URL: the document's ID,
// the number, the record and the stored document, each a 64-bit number, a byte
// of flags and one of the referrer policy.
constexpr size_t kTraversalOverhead = 4 * 8 + 2;

// How many pages BackForwardCacheImpl keeps, its "cache_size", and how long,
// kDefaultTimeToLiveInBackForwardCacheInSeconds, or for a page of a no-store
// response the "ttl" of CacheControlNoStoreEnterBackForwardCache.
constexpr size_t kBackForwardCacheSize = 6;
constexpr base::TimeDelta kTimeToLive = base::Seconds(600);
constexpr base::TimeDelta kNoStoreTimeToLive = base::Minutes(3);

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

// DocumentLoader::CommitNavigation() for a document of no response, a
// srcdoc's or an about:blank's: its entry of the Performance Timeline names
// its URL, fetched nothing, came from the cache with the bytes of its body,
// and ended as it committed, as Chrome 157 gives it.
void TimeLocalCommit(DocumentLoadTiming& timing,
                     const GURL& url,
                     size_t body_size,
                     bool reload) {
  const base::TimeTicks now = base::TimeTicks::Now();
  timing.has_response = true;
  timing.is_reload = reload;
  timing.fetch_start = base::TimeTicks();
  timing.response_end = now;
  ResourceTimingInfo navigation;
  navigation.name = base::UTF8ToUTF16(url.spec());
  navigation.allow_timing_details = true;
  navigation.cache_state = CacheState::kLocal;
  navigation.response_end = now;
  navigation.encoded_body_size = body_size;
  navigation.decoded_body_size = body_size;
  timing.navigation = std::move(navigation);
}

}  // namespace

PageFrame::LocalNavigation::LocalNavigation() = default;
PageFrame::LocalNavigation::LocalNavigation(LocalNavigation&&) = default;
PageFrame::LocalNavigation& PageFrame::LocalNavigation::operator=(
    LocalNavigation&&) = default;
PageFrame::LocalNavigation::~LocalNavigation() = default;

struct PageFrame::StoredDocument {
  StoredDocument() = default;
  StoredDocument(const StoredDocument&) = delete;
  StoredDocument& operator=(const StoredDocument&) = delete;
  ~StoredDocument() = default;

  // The window's context, with its own global proxy, and what runs in it.
  v8::Global<v8::Context> context;
  cppgc::Persistent<Window> window;
  std::unique_ptr<DOMTimerCoordinator> timers;
  std::unique_ptr<IdleTaskController> idle_tasks;
  std::unique_ptr<AnimationFrameController> animation_frames;
  std::unique_ptr<PageFetcher> fetcher;
  std::unique_ptr<WindowTasks> window_tasks;
  std::unique_ptr<PageWorkerStarter> worker_starter;
  raw_ptr<ChromeObjectData> chrome_object_data = nullptr;
  std::unique_ptr<DocumentLoader> loader;
  // Its ID, and the entry it was stored at, the only one that restores it.
  uint64_t document = kNoDocument;
  int64_t item_sequence_number = 0;
  scoped_refptr<FrameNavigationEntry> entry;
  uint64_t browsing_instance = 0;
  // The frame's name, which Chrome's frame of the page keeps with it.
  std::u16string name;
  base::OneShotTimer time_to_live;
};

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
  window_->BindToContext(isolate, context());
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
    // Its policy container is its creator's, as Chrome's browser gives a new
    // frame's, and so is its referrer policy.
    document.SetReferrerPolicy(creator.GetReferrerPolicy());
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
  if (provisional_ || pending_local_navigation_) {
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
  // HTMLFrameOwnerElement::ReferrerPolicyAttribute(): the iframe's
  // referrerpolicy, for the navigation's referrer.
  if (HTMLFrameOwnerElement* const owner = owner_.Get()) {
    if (const std::u16string* const policy =
            owner->FindAttribute(u"referrerpolicy")) {
      request.referrer_policy = FetchReferrerPolicyFromString(*policy);
    }
  }
  Navigate(*window_, request, FrameLoadType::kStandard);
}

void PageFrame::CommitWithoutNetwork(uint64_t number) {
  if (!pending_local_navigation_ ||
      pending_local_navigation_->number != number || !window_) {
    return;
  }
  LocalNavigation navigation = std::move(*pending_local_navigation_);
  pending_local_navigation_.reset();
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
    // DocumentLoader::CommitNavigation(): no script of the old document or
    // of its frames starts a navigation of this frame meanwhile.
    const FrameNavigationDisabler navigation_disabler(*this);
    {
      const v8::Context::Scope old_scope(context());
      if (current_) {
        current_->StopLoading();
      }
      old_document.DispatchUnloadEvents();
    }
    // A listener of the old document's unload events, or of its frames',
    // that removed the frame's element took the frame away, which then
    // commits nothing.
    if (detaching_) {
      return;
    }
    DetachChildren();
    if (detaching_) {
      return;
    }
  }
  Document* document = nullptr;
  CookieStore* const cookie_store =
      reuse_window ? window_->GetCookieStoreIfExists() : nullptr;
  if (reuse_window) {
    const v8::Context::Scope context_scope(context());
    if (cookie_store) {
      cookie_store->DocumentWillChange();
    }
    window_->SetDocumentId(SendDocumentCommitted(committed));
    document = Document::CreateHTML();
    window_->SetDocument(document);
  } else {
    // The frame keeps its WindowProxy, which takes the new window.
    const v8::Local<v8::Object> global_proxy = context()->Global();
    DetachWindow();
    context()->DetachGlobal();
    // The new window's time origin is the navigation's start, as Blink's is,
    // before the old document's unload events and the task that commits.
    const v8::Local<v8::Context> context =
        NewContext(navigation.start, origin,
                   /*cross_origin_isolated=*/false, global_proxy);
    context_.Reset(isolate, context);
    window_ = bindings::V8Window::ToWrappable(isolate, context->Global());
    window_->BindToContext(isolate, context);
    AttachWindow(parent_->window_->Connection(), committed);
    const v8::Context::Scope context_scope(context);
    document = Document::CreateHTML();
    window_->SetDocument(document);
  }
  current_.reset();
  const v8::Context::Scope context_scope(context());
  document->SetLoadTiming(&document_load_timing());
  TimeLocalCommit(document_load_timing(), url,
                  base::UTF16ToUTF8(navigation.body).size(), navigation.reload);
  document->SetUrl(base::UTF8ToUTF16(url.spec()));
  // HTMLDocument's constructor: a srcdoc document stays in no-quirks mode,
  // whatever its doctype.
  if (url.IsAboutSrcdoc()) {
    document->LockCompatibilityMode();
  }
  document->SetOrigin(origin);
  document->SetReferrer(navigation.referrer);
  document->SetReferrerPolicy(navigation.referrer_policy);
  document->SetAboutBaseUrl(navigation.about_base_url);
  // A srcdoc takes its parent's cookie URL as it commits, as Blink's
  // DocumentInit does from its owner document, and as the Session does.
  document->SetCookieUrl(url.IsAboutSrcdoc()
                             ? parent_->window_->GetDocument().CookieUrl()
                             : navigation.cookie_url);
  if (origin.GetURL().SchemeIsHTTPOrHTTPS()) {
    document->SetCookieJar(&runner_->cookie_jar());
    GiveStorage(*document);
  }
  if (cookie_store) {
    cookie_store->DocumentChanged();
  }
  // A child frame's document has an entry of its own, which none of the
  // session history's is until frames have entries there.
  window_->SetCurrentHistoryItem(HistoryItem::Create(url));
  TellOfLocalUrl(url);
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
  CheckParentLoadSoon();
}

void PageFrame::CheckParentLoadSoon() {
  if (!parent_) {
    return;
  }
  PageFrame& parent = *parent_;
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
  TimeLocalCommit(document_load_timing(), url, /*body_size=*/0,
                  /*reload=*/false);
  // It keeps the initial document's about base URL, as Chrome 157 does, not
  // its initiator's base URL of the time, and its cookie URL.
  document->SetAboutBaseUrl(old_document.FallbackBaseUrl());
  document->SetCookieUrl(old_document.CookieUrl());
  document->SetReferrerPolicy(old_document.GetReferrerPolicy());
  // The referrer is the full URL of the document that started it, its
  // parent's, as RenderFrameImpl takes it from the initiator frame.
  if (parent_ && parent_->window_ && parent_->window_->HasDocument()) {
    document->SetReferrer(parent_->window_->GetDocument().UrlForBinding());
  }
  window.SetDocument(document);
  window.SetCurrentHistoryItem(HistoryItem::Create(url));
  TellOfLocalUrl(url);
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
  // A navigation that a pagehide listener of a document that the back/forward
  // cache takes starts goes nowhere, a Known Gap: Chrome's goes on.
  if (IgnoresLeavingDocument() || leaving_ == Leaving::kStays) {
    return;
  }
  if (parent_) {
    // A srcdoc or an about:blank goes through Chrome's browser and commits
    // from a later task, with the srcdoc that an iframe has now. The newest
    // navigation takes the place of one that waits, whatever its URL. A
    // frame's navigations through the Session, and to a data: or blob: URL,
    // are Known Gaps for now: it keeps its document.
    CancelLocalNavigation();
    if (!request.url.IsAboutSrcdoc() && !request.url.IsAboutBlank()) {
      // The navigation aborted the document, which may have held the
      // parent's load event back, and starts no loader in its place.
      CheckParentLoadSoon();
      return;
    }
    LocalNavigation navigation;
    navigation.start = base::TimeTicks::Now();
    navigation.url = request.url;
    navigation.reload = request.reload;
    // Its document's referrer is what the initiator's outgoing referrer gives
    // a navigation to its URL under the initiator's referrer policy, as
    // Chrome's browser takes it from the navigation's request.
    navigation.referrer =
        base::UTF8ToUTF16(GenerateReferrer(request.referrer_policy, request.url,
                                           request.initiator_referrer)
                              .spec());
    if (request.url.IsAboutSrcdoc()) {
      navigation.committed.source = PageDocumentSource::kSrcdoc;
      // Only an iframe's srcdoc reaches its document, as Chrome's browser
      // takes it.
      HTMLFrameOwnerElement* const owner = owner_.Get();
      const std::u16string* const srcdoc =
          owner && owner->HasHTMLTagName(u"iframe")
              ? owner->FindAttribute(u"srcdoc")
              : nullptr;
      // A sandboxed frame's srcdoc would run with the flags that its sandbox
      // withholds, which the Page cannot enforce yet, so it commits nothing,
      // and the parent, whose load event the document that the navigation
      // aborted may have held back, checks again.
      if (IsSandboxed()) {
        CheckParentLoadSoon();
        return;
      }
      navigation.body = srcdoc ? *srcdoc : std::u16string();
      Document& parent_document = parent_->window_->GetDocument();
      navigation.origin = parent_document.GetOrigin();
      navigation.referrer_policy = parent_document.GetReferrerPolicy();
      navigation.about_base_url = parent_document.BaseUrl();
      navigation.cookie_url = parent_document.CookieUrl();
    } else {
      navigation.committed.source = PageDocumentSource::kAboutBlank;
      navigation.committed.initiator = request.initiator;
      // The Session takes what the about:blank inherits of its initiator now.
      runner_->send().Run(
          EncodeAboutBlankNavigation(window_->DocumentId(), request.initiator));
      navigation.origin = request.initiator_origin;
      navigation.referrer_policy = request.initiator_referrer_policy;
      navigation.about_base_url = request.initiator_base_url;
      navigation.cookie_url = request.initiator_cookie_url;
    }
    StopAllLoaders();
    navigation.number = ++last_local_navigation_;
    const uint64_t number = navigation.number;
    pending_local_navigation_ =
        std::make_unique<LocalNavigation>(std::move(navigation));
    EventLoop::Get()
        ->TaskRunner(TaskType::kInternalLoading)
        ->PostTask(FROM_HERE,
                   base::BindOnce(&PageFrame::CommitWithoutNetwork,
                                  weak_factory_.GetWeakPtr(), number));
    return;
  }
  // Navigator::OnBeginNavigation(): the browser ignores a navigation that a
  // renderer starts without the user's activation while a history
  // navigation waits.
  if (TraversalWaits()) {
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
  traversal_item_.reset();
  traversal_record_.reset();
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

void PageFrame::DidCommitSameDocumentNavigation(const HistoryItem& item,
                                                FrameLoadType type) {
  if (IgnoresLeavingDocument()) {
    return;
  }
  // The traversal has committed, as Chrome's controller drops its pending
  // entry once it hears of the commit, so what its popstate listeners and
  // their microtasks ask counts from the entry it went to.
  committing_traversal_index_.reset();
  // A child frame's commits stay its own until frames have entries of their
  // own.
  if (!parent_) {
    // The document whose navigation the entry repeats, none for one that no
    // navigation's response made, such as the tab's initial about:blank,
    // whose entries no traversal goes to.
    const uint64_t document = current_ ? window_->DocumentId() : kNoDocument;
    // The document's policy, Chrome's own where it has none, as the referrer
    // of a same-document commit goes with it.
    const FetchReferrerPolicy policy =
        window_->GetDocument().GetReferrerPolicy();
    history().DidCommitSameDocumentNavigation(
        item, type, document,
        policy == FetchReferrerPolicy::kDefault
            ? PageReferrerPolicy::kStrictOriginWhenCrossOrigin
            : ToPageReferrerPolicy(policy));
    ReleaseRecords(document);
    // A new entry discards the pending one, which a later go() no longer
    // counts from, as Chrome's controller discards it.
    if (type == FrameLoadType::kStandard) {
      pending_traversal_index_.reset();
    }
    NotifyNavigationApiOfDisposedEntries();
  }
  const std::string& spec = item.url.spec();
  // A URL too long for a frame stays the Page's, which no Chrome would have.
  if (spec.size() <= kMaxPayloadSize - 8) {
    runner_->send().Run(
        EncodeSameDocumentNavigation(window_->DocumentId(), spec));
  }
}

void PageFrame::TellOfLocalUrl(const GURL& url) {
  if (url == GURL(url::kAboutBlankURL) || url == GURL(url::kAboutSrcdocURL)) {
    return;
  }
  const std::string& spec = url.spec();
  // A URL too long for a frame stays the Page's, which no Chrome would have.
  if (spec.size() <= kMaxPayloadSize - 8) {
    runner_->send().Run(
        EncodeSameDocumentNavigation(window_->DocumentId(), spec));
  }
}

void PageFrame::DidOpenDocument(const Window& opener) {
  runner_->send().Run(
      EncodeDocumentOpened(window_->DocumentId(), opener.DocumentId()));
}

void PageFrame::NotifyNavigationApiOfDisposedEntries() {
  std::vector<std::u16string> keys = history().TakeRemovedKeys();
  if (keys.empty()) {
    return;
  }
  // From a later task, as Chrome's browser tells the renderer.
  PostHistoryTask(base::BindOnce(
      [](std::vector<std::u16string> keys) {
        Window::Current().Navigation()->DisposeEntriesForSessionHistoryRemoval(
            keys);
      },
      std::move(keys)));
}

void PageFrame::DidUpdateCurrentHistoryItem(const HistoryItem& item) {
  if (!parent_ && leaving_ != Leaving::kUnloads) {
    history().UpdateCurrentItem(item);
  }
}

HistoryCounts PageFrame::GetHistoryCounts() const {
  // The joint session history's, for every frame of the tab, as each of
  // Blink's frames reads its WebView's.
  return history().Counts();
}

size_t PageFrame::CurrentEntryIndex() const {
  // GetCurrentEntryIndex(): the pending entry, which a traversal that waits,
  // or one whose navigate event runs, goes to, or else the current one.
  if (pending_traversal_index_) {
    return *pending_traversal_index_;
  }
  if (committing_traversal_index_) {
    return *committing_traversal_index_;
  }
  return static_cast<size_t>(history().Counts().index);
}

bool PageFrame::TraversalWaits() const {
  // Also while its beforeunload and navigate events run, as Chrome's browser
  // has started the traversal's request by then.
  return (provisional_ && traversal_item_) || pinned_record_;
}

void PageFrame::NavigateBackForward(int offset) {
  // The joint session history moves, which only its top-level frame has
  // entries in so far.
  if (parent_) {
    Top().NavigateBackForward(offset);
    return;
  }
  // Nothing traverses while a new document commits, as FrameLoader's
  // FrameNavigationDisabler has it while the old document's frames unload.
  if (!IsNavigationAllowed()) {
    return;
  }
  // GoToOffsetFromRenderer(): from the entry that a waiting traversal goes
  // to, or the current one, and nowhere past the entries.
  const HistoryCounts counts = history().Counts();
  const int index = static_cast<int>(CurrentEntryIndex()) + offset;
  if (index < 0 || index >= counts.length) {
    return;
  }
  // As a navigation of a document that leaves.
  if (IgnoresLeavingDocument() || leaving_ == Leaving::kStays) {
    return;
  }
  // RenderFrameHostImpl::GoToEntryAtOffset(): ignored while a history
  // navigation waits, without the user's activation.
  if (TraversalWaits()) {
    return;
  }
  QueueTraversal(static_cast<size_t>(index), std::nullopt);
}

void PageFrame::NavigateToNavigationApiKey(const std::u16string& key) {
  // RenderFrameHostImpl::NavigateToNavigationApiKey(): ignored while a
  // history navigation waits, without the user's activation.
  if (!Top().IsNavigationAllowed() || Top().IgnoresLeavingDocument() ||
      Top().leaving_ == Leaving::kStays || Top().TraversalWaits()) {
    return;
  }
  // A child frame's entries are none of the session history's yet.
  const std::optional<size_t> index =
      parent_ ? std::nullopt : history().IndexOfKey(key, CurrentEntryIndex());
  if (!index) {
    PostHistoryTask(base::BindOnce(
        [](std::u16string key) {
          Window::Current().Navigation()->TraverseCancelled(
              key, Navigation::TraverseCancelledReason::kNotFound);
        },
        key));
    return;
  }
  QueueTraversal(*index, key);
}

void PageFrame::QueueTraversal(size_t index,
                               std::optional<std::u16string> key) {
  pending_traversal_index_ = index;
  ++waiting_traversals_;
  // Behind the tasks that the document has queued, hashchange among them, as
  // the browser's answer comes in Chrome.
  PostHistoryTask(base::BindOnce(
      &PageFrame::GoToItem, base::Unretained(this),
      history().RootAt(index).item().item_sequence_number, std::move(key)));
}

void PageFrame::PostHistoryTask(base::OnceClosure task) {
  EventLoop::Get()
      ->TaskRunner(TaskType::kDOMManipulation)
      ->PostTask(
          FROM_HERE,
          base::BindOnce(
              &ScriptRunner::RunTask, base::Unretained(&*runner_),
              base::BindOnce(&PageFrame::RunInCurrentWindow,
                             weak_factory_.GetWeakPtr(), std::move(task))));
}

void PageFrame::RunInCurrentWindow(base::OnceClosure task) {
  // A frame that goes runs nothing more.
  if (detaching_ || context_.IsEmpty()) {
    return;
  }
  const v8::Context::Scope context_scope(context());
  std::move(task).Run();
}

void PageFrame::GoToItem(int64_t item_sequence_number,
                         std::optional<std::u16string> key) {
  if (--waiting_traversals_ == 0) {
    pending_traversal_index_.reset();
  }
  // ShouldIgnoreIncomingRendererRequest(): the browser takes a traversal
  // that the page asked for as it comes, and ignores it while one to another
  // document that an earlier ask started waits for its response.
  if (TraversalWaits()) {
    return;
  }
  const std::optional<size_t> index =
      history().IndexOfItem(item_sequence_number);
  if (!index) {
    if (key) {
      Window::Current().Navigation()->TraverseCancelled(
          *key, Navigation::TraverseCancelledReason::kAbortedBeforeCommit);
    }
    return;
  }
  const base::AutoReset<std::optional<size_t>> committing(
      &committing_traversal_index_, *index);
  if (!GoToIndex(*index) && key) {
    // A traversal that went nowhere ends the traverseTo() that asked for it.
    Window::Current().Navigation()->TraverseCancelled(
        *key, Navigation::TraverseCancelledReason::kAbortedBeforeCommit);
  }
}

bool PageFrame::GoToIndex(size_t index) {
  Window& window = Window::Current();
  const HistoryItem* const current = window.CurrentHistoryItem();
  if (!current || !window.HasContext()) {
    return false;
  }
  const HistoryItem target = history().RootAt(index).item();
  if (target.document_sequence_number == current->document_sequence_number) {
    // The document commits it, and the history hears of it then.
    CommitSameDocumentHistoryNavigation(window, target);
    return true;
  }
  return BeginTraversal(history().RootAt(index));
}

bool PageFrame::BeginTraversal(const FrameNavigationEntry& entry) {
  // What the entry holds, before any listener can change the history.
  const HistoryItem target = entry.item();
  PageTraversalRequest traversal;
  traversal.record = entry.record();
  traversal.same_document_entry = entry.same_document_entry();
  traversal.referrer_policy = entry.referrer_policy();
  traversal.url = target.url.spec();
  // The tab's initial entry holds no navigation, and no traversal goes there,
  // nor to a URL that a Page message cannot carry, which no Chrome would have.
  if (traversal.record == kNoDocument ||
      traversal.url.size() > kMaxPayloadSize - kTraversalOverhead) {
    return false;
  }
  // The Session keeps the record while the traversal needs it, though a
  // listener's new entry prunes the entry meanwhile, or starts a navigation,
  // as Chrome's request holds its FrameNavigationEntry.
  const base::AutoReset<std::optional<uint64_t>> pinned(&pinned_record_,
                                                        traversal.record);
  Window& window = Window::Current();
  Document& document = window.GetDocument();
  if (document.PageDismissalEventBeingDispatched()) {
    return false;
  }
  // Navigator::Navigate(): the browser runs the document's beforeunload
  // first, which a page without the user's activation cannot cancel.
  if (!document.DispatchBeforeUnloadEvent() || !window.HasContext()) {
    return false;
  }
  // It takes the place of a navigation that waits, which ends with
  // ERR_ABORTED. The document keeps loading until the response commits, as
  // Chrome's browser tells its renderer nothing of the traversal till then.
  if (provisional_) {
    runner_->requests().Cancel(provisional_->number());
    provisional_.reset();
  }
  provisional_ = std::make_unique<DocumentLoader>(runner_->requests(),
                                                  runner_->send(), *this);
  provisional_request_ = NavigationRequest();
  provisional_request_.url = target.url;
  provisional_start_ = base::TimeTicks::Now();
  provisional_cookie_writes_ = runner_->cookie_jar().cookie_writes();
  traversal_item_ = target;
  traversal_record_ = traversal.record;
  traversal_browsing_instance_ = BrowsingInstanceOf(traversal.record);
  traversal_start_ = provisional_start_;
  EndRestore();
  // The entry that a stored document was stored at restores it, if the
  // Session agrees, and another entry of its document evicts it, as Chrome
  // evicts a page whose browsing instance a traversal goes to.
  if (const StoredDocument* const stored =
          FindStoredDocument(target.item_sequence_number)) {
    traversal.stored_document = stored->document;
    restore_document_ = stored->document;
  } else {
    // EvictFramesInRelatedSiteInstances(): the pages whose browsing instance
    // the traversal goes into, this one's entry among them.
    EvictBrowsingInstance(traversal_browsing_instance_);
  }
  provisional_->SetNumber(
      runner_->requests().StartTraversal(std::move(traversal), *provisional_));
  // MaybeDispatchNavigateEventForCrossDocumentTraversal(): for an entry of
  // the document's origin only, once the request has started, as Chrome's
  // browser tells the renderer of the traversal without waiting for it.
  if (document.GetOrigin().IsSameOriginWith(url::Origin::Create(target.url))) {
    Navigation::NavigateParams params;
    params.url = target.url;
    params.load_type = FrameLoadType::kBackForward;
    params.destination_item = &target;
    params.cross_document = true;
    window.Navigation()->DispatchNavigateEvent(params);
  }
  return true;
}

void PageFrame::ReleaseRecords(uint64_t live_document) {
  std::set<uint64_t> held = history().RecordsHeld();
  if (live_document != kNoDocument) {
    held.insert(live_document);
  }
  for (const std::optional<uint64_t>& record :
       {traversal_record_, pinned_record_}) {
    if (record) {
      held.insert(*record);
    }
  }
  // A stored document's own, which no entry may hold though one restores it,
  // repeats its navigation for the entries that it makes once it is back.
  for (const std::unique_ptr<StoredDocument>& stored : stored_documents_) {
    held.insert(stored->document);
  }
  std::vector<uint64_t> released;
  for (const uint64_t record : held_records_) {
    if (!held.contains(record)) {
      released.push_back(record);
    }
  }
  std::erase_if(browsing_instances_, [&held](const auto& entry) {
    return !held.contains(entry.first);
  });
  held_records_ = std::move(held);
  if (!released.empty()) {
    runner_->send().Run(EncodeRecordsReleased(released));
  }
}

NavigationRateLimiter& PageFrame::GetNavigationRateLimiter() {
  return navigation_rate_limiter_;
}

SessionHistory& PageFrame::history() const {
  return runner_->session_history();
}

PageFrame& PageFrame::Top() {
  PageFrame* top = this;
  while (top->parent_) {
    top = top->parent_;
  }
  return *top;
}

bool PageFrame::HasProvisionalNavigation() const {
  // A srcdoc or an about:blank that waits to commit is the browser's
  // navigation that FrameLoader's client navigation stands for.
  return provisional_ != nullptr || pending_local_navigation_ != nullptr;
}

void PageFrame::StopAllLoaders() {
  if (current_) {
    current_->StopLoading();
  }
  if (provisional_) {
    runner_->requests().Cancel(provisional_->number());
    provisional_.reset();
  }
  traversal_item_.reset();
  traversal_record_.reset();
  EndRestore();
  CancelLocalNavigation();
}

void PageFrame::CancelLocalNavigation() {
  if (!pending_local_navigation_) {
    return;
  }
  pending_local_navigation_.reset();
  CheckParentLoadSoon();
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
  // How the navigation goes in the session history, the opening one in
  // place of the initial entry, and the entry it comes from.
  FrameLoadType type = FrameLoadType::kReplaceCurrentItem;
  // The entry that a traversal goes to, which the commit takes before any
  // listener runs.
  std::optional<HistoryItem> traversal_target;
  if (committing->number() != kNavigationRequest) {
    traversal_target = std::exchange(traversal_item_, std::nullopt);
  }
  const bool traversal = traversal_target.has_value();
  if (traversal) {
    type = FrameLoadType::kBackForward;
  } else if (committing->number() != kNavigationRequest) {
    type = provisional_request_.reload    ? FrameLoadType::kReload
           : provisional_request_.replace ? FrameLoadType::kReplaceCurrentItem
                                          : FrameLoadType::kStandard;
  }
  pending_traversal_index_.reset();
  const HistoryItem* const old_item = Window::Current().CurrentHistoryItem();
  const std::optional<HistoryItem> old_history_item =
      old_item ? std::optional<HistoryItem>(*old_item) : std::nullopt;
  // A traversal that may have restored a stored document has a response
  // instead, as the Session would not restore it: the document goes.
  EndRestore();
  // The next document's browsing instance: a traversal's entry's, a new one
  // where the browser swaps proactively, and otherwise the current one's.
  const uint64_t next_browsing_instance =
      traversal ? traversal_browsing_instance_
      : SwapsBrowsingInstance(old_document, type, url)
          ? ++last_browsing_instance_
          : browsing_instance_;
  const bool may_store = CanStoreDocument(old_document, next_browsing_instance);
  // A document that the back/forward cache takes has pagehide, persisted,
  // and visibilitychange in place of its unload events, as Chrome's renderer
  // dispatches them for the old page as the new one commits, and keeps
  // loading. What its listeners do to its URL and entries is its entry's,
  // until the browser has it in the cache, where it freezes.
  const bool store =
      may_store && LeaveForBackForwardCache(
                       old_document, next_browsing_instance, Leaving::kStays);
  // What the old document's unload listeners do to its URL and entries goes
  // nowhere, as Chrome's browser ignores a document that it is replacing, and
  // nothing after them runs a script.
  const base::AutoReset<Leaving> leaving(&leaving_, Leaving::kUnloads);
  // DocumentLoader::CommitNavigation(): nothing navigates the frame
  // meanwhile, whatever the old document or its frames' listeners ask.
  const FrameNavigationDisabler navigation_disabler(*this);
  if (may_store) {
    // It had its events.
  } else if (old_document.IsInitialEmptyDocument()) {
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
  const GURL old_url(old_document.UrlForBinding());
  const base::TimeTicks unload_event_start = old_document.UnloadEventStart();
  const base::TimeTicks unload_event_end = old_document.UnloadEventEnd();
  if (store) {
    StoreDocument(std::exchange(current_, std::move(committing)));
  } else {
    current_ = std::move(committing);
  }
  Document& document =
      CommitDocument(current_->number(), url, navigation_start, connection,
                     cross_origin_isolated, xml_content_type);
  browsing_instance_ = next_browsing_instance;
  browsing_instances_[document.DefaultView()->DocumentId()] =
      next_browsing_instance;
  // The old document's unload times go to a new one of its origin, as
  // Blink's UnloadEventTimingInfo carries them.
  DocumentLoadTiming& timing = document_load_timing();
  timing.unload_event_start = unload_event_start;
  timing.unload_event_end = unload_event_end;
  timing.has_same_origin_as_previous_document =
      url::IsSameOriginWith(old_url, url);
  timing.is_reload = !traversal && current_->number() != kNavigationRequest &&
                     provisional_request_.reload;
  timing.is_back_forward = traversal;
  // The new document's entry, which the session history takes, before any of
  // its scripts runs. The isolate's current context is the old document's
  // again here.
  // A traversal's document takes the entry's own item, its state, key, ID and
  // scrollRestoration among it, at the URL its response came from.
  HistoryItem item;
  int64_t traversed_item_sequence_number = 0;
  if (traversal) {
    traversed_item_sequence_number = traversal_target->item_sequence_number;
    // A redirect leaves nothing of the entry for the response: no state, and
    // a new key, ID and document, as Chrome's NavigationRequest resets the
    // entry's page state on any redirect, also one that ends at its URL.
    item = current_->redirected() ? HistoryItem::Create(url)
                                  : *std::move(traversal_target);
    item.url = url;
    // Whether it protects its URL is the new document's, as its response's
    // policy says once it commits, as for any new document in Chrome.
    item.protect_url_in_navigation_api = false;
    traversal_record_.reset();
  } else {
    item = HistoryItem::ForCommit(
        old_history_item ? &*old_history_item : nullptr, url, type,
        HistoryNavigationType::kDifferentDocument);
  }
  Window& window = *document.DefaultView();
  Navigation::EntryArrays arrays = history().DidCommitNavigation(
      item, type, window.DocumentId(), traversed_item_sequence_number);
  // The new document's entries come from the history as it is, without the
  // entries that it dropped, which no other document shows.
  history().TakeRemovedKeys();
  window.SetCurrentHistoryItem(item);
  window.Navigation()->InitializeForNewWindow(std::move(arrays));
  ReleaseRecords(window.DocumentId());
  return document;
}

void PageFrame::RestoreDocument(DocumentLoader& loader) {
  v8::Isolate* const isolate = runner_->isolate();
  const v8::HandleScope handle_scope(isolate);
  // The traversal's loader is on the stack below this, so it goes from a
  // later task.
  base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE,
                                                             Take(loader));
  // The Page keeps the document that a traversal may restore until it ends,
  // so the Session restores only what is here.
  CHECK(restore_document_);
  const uint64_t restored = *std::exchange(restore_document_, std::nullopt);
  const auto found = std::ranges::find_if(
      stored_documents_,
      [restored](const auto& stored) { return stored->document == restored; });
  CHECK(found != stored_documents_.end());
  std::unique_ptr<StoredDocument> stored = std::move(*found);
  stored_documents_.erase(found);
  traversal_item_.reset();
  pending_traversal_index_.reset();
  // The Session keeps the document's own record, and the entry's, until the
  // history is at the entry again.
  pinned_record_ = restored;
  {
    // The current document leaves as at any commit, into the cache, or it
    // unloads, but the browser has committed the restore already, so what it
    // does to its URL and entries goes nowhere.
    const v8::Context::Scope current_scope(context());
    Document& old_document = window_->GetDocument();
    const bool may_store =
        CanStoreDocument(old_document, stored->browsing_instance);
    const bool store = may_store && LeaveForBackForwardCache(
                                        old_document, stored->browsing_instance,
                                        Leaving::kCached);
    const base::AutoReset<Leaving> leaving(&leaving_, Leaving::kUnloads);
    const FrameNavigationDisabler navigation_disabler(*this);
    if (!may_store) {
      if (current_) {
        current_->StopLoading();
      }
      old_document.DispatchUnloadEvents();
    }
    runner_->TopLevelContextGoes();
    if (store) {
      StoreDocument(std::move(current_));
    } else {
      DetachChildren();
      DetachWindow();
      context()->DetachGlobal();
    }
  }
  // RestoreEntry(): the stored window becomes the frame's again.
  context_ = std::move(stored->context);
  window_ = stored->window.Get();
  timers_ = std::move(stored->timers);
  idle_tasks_ = std::move(stored->idle_tasks);
  animation_frames_ = std::move(stored->animation_frames);
  fetcher_ = std::move(stored->fetcher);
  window_tasks_ = std::move(stored->window_tasks);
  worker_starter_ = std::move(stored->worker_starter);
  chrome_object_data_ = stored->chrome_object_data;
  name_ = std::move(stored->name);
  current_ = std::move(stored->loader);
  browsing_instance_ = stored->browsing_instance;
  MemoryInfo::SetProcessLockedToSite(true);
  const v8::Local<v8::Context> context = this->context();
  const v8::Context::Scope context_scope(context);
  Window& window = *window_;
  window.SetFrame(this);
  window.SetNavigationClient(this);
  window.SetBackForwardCacheEviction(base::RepeatingClosure());
  runner_->sender()->SendDocumentRestored(window.DocumentId());
  // The Session's storage answers the restored document again, as its hosts
  // had followed the one before it.
  SetStorageDocument(window.DocumentId());
  // The session history goes to the document's entry, and the Navigation API
  // takes the entries around it while the page is still frozen.
  Navigation::EntryArrays arrays =
      history().DidRestoreDocument(std::move(stored->entry));
  traversal_record_.reset();
  pinned_record_.reset();
  window.Navigation()->SetEntriesForRestore(arrays);
  // RemoveBackForwardCacheEviction(), then SetPageFrozen(false): what waited
  // runs from its tasks, the responses that came meanwhile among them.
  window.SetIsInBackForwardCache(false);
  for (auto& [type, task] : window.TakeHeldTasks()) {
    PostTaskInContext(&window, type, std::move(task));
  }
  timers_->Resume();
  idle_tasks_->Resume();
  animation_frames_->Resume();
  window_tasks_->Resume();
  EventLoop::Get()
      ->TaskRunner(TaskType::kNetworkingUnfreezable)
      ->PostTask(FROM_HERE,
                 base::BindOnce(
                     &ScriptRunner::RunTask, base::Unretained(&*runner_),
                     base::BindOnce(&PageRequests::DeliverHeld,
                                    base::Unretained(&runner_->requests()))));
  // LocalFrame::DidResume()'s resume, then the page shows again, then its
  // pageshow, persisted, with the traversal's start as its time.
  Document& document = window.GetDocument();
  document.DispatchResumeEvent();
  document.ShowFromBackForwardCache();
  document.DispatchPersistedPageshowEvent(traversal_start_);
  // Document::EnqueuePageRevealEvent(): pagereveal in the page's first frame
  // again, before its callbacks, as WebViewImpl enqueues it once the page
  // shows.
  window.EnqueueAnimationFrameEvent(window, *PageRevealEvent::CreateTrusted());
  ReleaseRecords(window.DocumentId());
}

bool PageFrame::LeaveForBackForwardCache(Document& document,
                                         uint64_t next_browsing_instance,
                                         Leaving leaving) {
  left_page_evicted_ = false;
  {
    const base::AutoReset<Leaving> hiding(&leaving_, leaving);
    document.HideForBackForwardCache();
  }
  if (window_) {
    const base::AutoReset<Leaving> freezing(&leaving_, Leaving::kCached);
    document.DispatchFreezeEvent();
  }
  if (window_ && !left_page_evicted_ &&
      CanStoreDocument(document, next_browsing_instance)) {
    return true;
  }
  if (current_) {
    current_->StopLoading();
  }
  return false;
}

bool PageFrame::IgnoresLeavingDocument() {
  if (leaving_ == Leaving::kCached) {
    left_page_evicted_ = true;
  }
  return leaving_ == Leaving::kCached || leaving_ == Leaving::kUnloads;
}

bool PageFrame::MayStoreDocument(const Document& document) const {
  // PopulateReasonsForMainDocument(): a response of status 200, to a GET,
  // which each navigation of a Page is, and no unload listener, which the
  // window has where the seed does not deprecate unload for its document.
  // The sticky features that a Page has: a parser that was stopped while it
  // parsed, and, for a no-store response, a script's request whose response
  // said no-store too.
  return current_ && window_ && current_->status() == 200 &&
         !window_->HasEventListeners(u"unload") && !document.ParserAborted() &&
         !(current_->no_store() && window_->ReceivedNoStoreForScriptRequest());
}

bool PageFrame::SwapsBrowsingInstance(const Document& document,
                                      FrameLoadType type,
                                      const GURL& url) const {
  // From a tab's page of an HTTP or HTTPS URL, which the cache may keep, and
  // neither in place of the entry, nor a reload, nor to the same URL but for
  // its fragment.
  const GURL document_url(document.UrlForBinding());
  return !parent_ && !document.IsInitialEmptyDocument() &&
         document_url.SchemeIsHTTPOrHTTPS() &&
         type != FrameLoadType::kReplaceCurrentItem &&
         type != FrameLoadType::kReload &&
         !document_url.EqualsIgnoringRef(url) && MayStoreDocument(document);
}

bool PageFrame::CanStoreDocument(const Document& document,
                                 uint64_t next_browsing_instance) const {
  // A tab's page, which a navigation's response loaded, whose browsing
  // instance the next page does not share: kBrowsingInstanceNotSwapped.
  if (parent_ || !current_ || !window_ || document.IsInitialEmptyDocument() ||
      next_browsing_instance == browsing_instance_ ||
      !GURL(document.UrlForBinding()).SchemeIsHTTPOrHTTPS() ||
      !MayStoreDocument(document)) {
    return false;
  }
  // PopulateNonStickyReasonsForDocument(): not before DOMContentLoaded.
  if (!chrome_object_data_ || chrome_object_data_->load_timing
                                  ->dom_content_loaded_event_end.is_null()) {
    return false;
  }
  // Known Gaps: the cache keeps no frames and freezes no workers yet, and
  // the Session keeps a top-level document's Cache API, IndexedDB, Storage
  // Buckets and CookieStore listeners only while it is the current one.
  if (!children_.empty() ||
      (worker_starter_ && worker_starter_->HasStarted())) {
    return false;
  }
  const CacheStorageHost* const cache_storage = cache_storage_host();
  const IndexedDbHost* const indexed_db = indexed_db_host();
  const StorageBucketHost* const storage_buckets = storage_bucket_host();
  const CookieStore* const cookie_store = window_->GetCookieStoreIfExists();
  return !(cache_storage && cache_storage->UsedByDocument()) &&
         !(indexed_db && indexed_db->UsedByDocument()) &&
         !(storage_buckets && storage_buckets->UsedByDocument()) &&
         !(cookie_store && cookie_store->IsObserving());
}

uint64_t PageFrame::BrowsingInstanceOf(uint64_t document) {
  const auto found = browsing_instances_.find(document);
  return found != browsing_instances_.end() ? found->second
                                            : ++last_browsing_instance_;
}

void PageFrame::StoreDocument(std::unique_ptr<DocumentLoader> loader) {
  v8::Isolate* const isolate = runner_->isolate();
  const v8::HandleScope handle_scope(isolate);
  auto stored = std::make_unique<StoredDocument>();
  Window& window = *window_;
  stored->document = window.DocumentId();
  stored->browsing_instance = browsing_instance_;
  stored->item_sequence_number =
      window.CurrentHistoryItem()
          ? window.CurrentHistoryItem()->item_sequence_number
          : 0;
  stored->entry = history().CurrentRootRef();
  stored->name = name_;
  stored->context = std::move(context_);
  stored->window = window_.Get();
  window_ = nullptr;
  stored->timers = std::move(timers_);
  stored->idle_tasks = std::move(idle_tasks_);
  stored->animation_frames = std::move(animation_frames_);
  stored->fetcher = std::move(fetcher_);
  stored->window_tasks = std::move(window_tasks_);
  stored->worker_starter = std::move(worker_starter_);
  stored->chrome_object_data = std::exchange(chrome_object_data_, nullptr);
  const bool no_store = loader->no_store();
  stored->loader = std::move(loader);
  const uint64_t document = stored->document;
  {
    // It is no frame's window while the cache keeps it, and evicts itself
    // once a script tries to run in it.
    const v8::Context::Scope context_scope(stored->context.Get(isolate));
    window.SetFrame(nullptr);
    window.SetNavigationClient(nullptr);
    window.SetIsInBackForwardCache(true);
    window.SetBackForwardCacheEviction(base::BindRepeating(
        [](base::WeakPtr<PageFrame> frame, uint64_t document) {
          // From a task, as the script that tried to run is on the stack.
          base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
              FROM_HERE,
              base::BindOnce(&PageFrame::EvictDocument, frame, document));
        },
        weak_factory_.GetWeakPtr(), document));
  }
  Freeze(*stored);
  // The Session keeps its context, before it hears of the next document.
  runner_->sender()->SendDocumentStored(document);
  stored->time_to_live.Start(FROM_HERE,
                             no_store ? kNoStoreTimeToLive : kTimeToLive,
                             base::BindOnce(&PageFrame::EvictDocument,
                                            base::Unretained(this), document));
  stored_documents_.push_front(std::move(stored));
  // BackForwardCacheImpl::EnforceCacheSizeLimit(): the oldest go past the
  // limit, but one that a traversal may restore.
  while (stored_documents_.size() > kBackForwardCacheSize) {
    const auto oldest = std::ranges::find_if(
        stored_documents_.rbegin(), stored_documents_.rend(),
        [this](const auto& stored) {
          return stored->document != restore_document_;
        });
    if (oldest == stored_documents_.rend()) {
      break;
    }
    EvictDocument((*oldest)->document);
  }
}

void PageFrame::Freeze(StoredDocument& stored) {
  stored.timers->Freeze();
  stored.idle_tasks->Freeze();
  stored.animation_frames->Freeze();
  stored.window_tasks->Freeze();
}

PageFrame::StoredDocument* PageFrame::FindStoredDocument(
    int64_t item_sequence_number) {
  for (const std::unique_ptr<StoredDocument>& stored : stored_documents_) {
    if (stored->item_sequence_number == item_sequence_number) {
      return stored.get();
    }
  }
  return nullptr;
}

void PageFrame::EvictDocument(uint64_t document) {
  if (document == restore_document_) {
    return;
  }
  const auto found = std::ranges::find_if(
      stored_documents_,
      [document](const auto& stored) { return stored->document == document; });
  if (found == stored_documents_.end()) {
    return;
  }
  std::unique_ptr<StoredDocument> stored = std::move(*found);
  stored_documents_.erase(found);
  v8::Isolate* const isolate = runner_->isolate();
  const v8::HandleScope handle_scope(isolate);
  const v8::Local<v8::Context> context = stored->context.Get(isolate);
  Window& window = *stored->window;
  {
    // Its requests go, and what came of them meanwhile. What waits in its
    // queues goes with it.
    const v8::Context::Scope context_scope(context);
    window.SetBackForwardCacheEviction(base::RepeatingClosure());
    window.SetIsInBackForwardCache(false);
    window.TakeHeldTasks();
    // A keepalive request outlives the page, as it would outlive an unload,
    // which its fetcher lets go, before what else the window asked for goes.
    stored->fetcher->Stop();
    runner_->requests().CancelAllOf(window);
    window.SetTimers(nullptr);
    window.SetIdleTasks(nullptr);
    window.SetAnimationFrames(nullptr);
    window.SetFetcher(nullptr);
    window.SetTasks(nullptr);
    window.SetWorkerStarter(nullptr);
  }
  runner_->requests().DeliverHeld();
  stored->timers->Stop();
  stored->idle_tasks->Stop();
  stored->animation_frames->Stop();
  stored->window_tasks->Stop();
  stored->loader->StopLoading();
  runner_->reporter().ContextDestroyed(context);
  stored->chrome_object_data->detached = true;
  runner_->sender()->SendDocumentEvicted(document);
  // What ran in it may be on the stack, so it goes from a later task.
  base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE,
                                                             std::move(stored));
}

void PageFrame::EvictBrowsingInstance(uint64_t browsing_instance) {
  std::vector<uint64_t> documents;
  for (const std::unique_ptr<StoredDocument>& stored : stored_documents_) {
    if (stored->browsing_instance == browsing_instance) {
      documents.push_back(stored->document);
    }
  }
  for (const uint64_t document : documents) {
    EvictDocument(document);
  }
}

void PageFrame::EndRestore() {
  if (const std::optional<uint64_t> document =
          std::exchange(restore_document_, std::nullopt)) {
    EvictDocument(*document);
  }
}

void PageFrame::NavigationEndedWithoutDocument(DocumentLoader& loader) {
  if (provisional_.get() == &loader) {
    traversal_item_.reset();
    traversal_record_.reset();
    EndRestore();
  }
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
  // it. The frame keeps its WindowProxy, which takes the new window, as
  // LocalWindowProxy detaches the old global object from it.
  DetachChildren();
  // A window that the back/forward cache took keeps its context and its
  // global proxy, and the new window gets a WindowProxy of its own, as
  // Chrome's proactive browsing instance swap gives the next page a new
  // frame.
  v8::Local<v8::Object> global_proxy;
  if (!context_.IsEmpty()) {
    global_proxy = context()->Global();
    DetachWindow();
    context()->DetachGlobal();
  }
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
      NewContext(time_origin, url::Origin::Create(url), cross_origin_isolated,
                 global_proxy);
  context_.Reset(isolate, context);
  window_ = bindings::V8Window::ToWrappable(isolate, context->Global());
  window_->BindToContext(isolate, context);
  PageDocumentCommitted committed;
  committed.source = PageDocumentSource::kNavigationResponse;
  committed.navigation = navigation;
  AttachWindow(connection, committed);
  const v8::Context::Scope context_scope(context);
  Document* const document =
      xml_content_type
          ? static_cast<Document*>(XMLDocument::Create(*xml_content_type))
          : Document::CreateHTML();
  document->SetUrl(base::UTF8ToUTF16(url.spec()));
  // Blink's Document::CookieURL() is the URL it was made at, which a
  // navigation within it leaves, as the Session keeps it too.
  document->SetCookieUrl(url);
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
  // token, so that V8 lets each reach the others' objects. Blink gives an
  // initial empty document V8's own, its global object, so that each access
  // asks its CanAccess(), which lets one of the same origin through and tells
  // the browser: a Page has no browser to tell, and nothing yet that asks
  // across origins (#286), so it shares its origin's token.
  // V8 compares tokens as objects, so the token is an internalized string,
  // the same object for the same text, as Blink's V8AtomicString is.
  if (!origin.opaque()) {
    const std::string token = origin.Serialize();
    context->SetSecurityToken(
        v8::String::NewFromUtf8(isolate, token.data(),
                                v8::NewStringType::kInternalized,
                                base::checked_cast<int>(token.size()))
            .ToLocalChecked());
  } else if (const v8::Local<v8::Value> token = TokenOfOpaqueOrigin(origin);
             !token.IsEmpty()) {
    // Blink gives a context of an opaque origin V8's own token, and its
    // CanAccess() lets the same opaque origin through, which a frame's
    // document has when it takes its parent's or its initiator's. Here the
    // context takes the token of one whose document has that origin, so that
    // every context of an opaque origin shares the first one's. One of
    // another opaque origin keeps its own.
    context->SetSecurityToken(token);
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
  SetStorageDocument(committed.document);
  return committed.document;
}

void PageFrame::SetStorageDocument(uint64_t document) {
  if (CacheStorageHost* const host = cache_storage_host()) {
    host->DocumentCommitted(document);
  }
  if (StorageBucketHost* const host = storage_bucket_host()) {
    host->DocumentCommitted(document);
  }
  if (IndexedDbHost* const host = indexed_db_host()) {
    host->DocumentCommitted(document);
  }
}

v8::Local<v8::Value> PageFrame::TokenOfOpaqueOrigin(
    const url::Origin& origin) const {
  const PageFrame* top = this;
  while (top->parent_) {
    top = top->parent_;
  }
  std::vector<const PageFrame*> frames = {top};
  while (!frames.empty()) {
    const PageFrame* const frame = frames.back();
    frames.pop_back();
    if (frame->window_ && frame->window_->HasDocument() &&
        frame->window_->GetDocument().GetOrigin() == origin) {
      return frame->context()->GetSecurityToken();
    }
    for (const std::unique_ptr<PageFrame>& child : frame->children_) {
      frames.push_back(child.get());
    }
  }
  return v8::Local<v8::Value>();
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
    // The tab's initial entry, which the first navigation replaces, and a
    // child frame's own about:blank, which has none in the session history
    // until frames have entries of their own.
    window.SetCurrentHistoryItem(
        parent_ ? HistoryItem::Create(GURL(url::kAboutBlankURL))
                : history().CurrentRoot().item());
  }
  window.SetStart(runner_->start(), connection);
  window.SetDocumentId(document);
  window.SetDocumentLoadTiming(chrome_object_data_->load_timing.Get());
  window.SetConsoleSink(base::BindRepeating(
      [](const SendFrame& send, ConsoleLevel level, std::string text) {
        PageConsoleMessage message;
        message.level = level;
        // A message may hold what a page gave, such as the value that an
        // enumeration lacks, and goes no further than a Page message can.
        base::TruncateUTF8ToByteSize(text, kMaxConsoleTextSize, &message.text);
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
