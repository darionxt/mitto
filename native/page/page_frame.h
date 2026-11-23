// A frame of a Page, on the Page program's main thread: its window's context,
// with what the window runs in it, and the loader of its document and that of
// the navigation that waits for its response, as Blink's LocalFrame,
// LocalWindowProxy and FrameLoader, and content's RenderFrameImpl, keep them.
// Each document that the frame loads gets a window in a context of its own,
// whose timers, idle callbacks, animation frames, fetches, tasks and workers
// the frame keeps until the next document takes its place.
//
// It is each of its windows' NavigationClient: a navigation that a document
// starts goes to the Session as a request of Kind navigation, and its
// response, unless it has none, takes the place of the document once the old
// one has unloaded, as Blink's FrameLoader::CommitNavigation() has it.
//
// A Page's frames make a tree: the top-level frame, which the Page opens
// with, and a child for each iframe or frame element of a frame's document,
// which holds the child as long as the element is in that document.
//
// Upstream: third_party/blink/renderer/core/frame/local_frame.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/local_window_proxy.cc
// Upstream: third_party/blink/renderer/core/loader/frame_loader.cc
// Upstream: content/renderer/render_frame_impl.cc

#ifndef MITTO_PAGE_PAGE_FRAME_H_
#define MITTO_PAGE_PAGE_FRAME_H_

#include <stdint.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "mitto/dom/document_parser.h"
#include "mitto/dom/fetcher.h"
#include "mitto/dom/frame.h"
#include "mitto/dom/navigation_client.h"
#include "mitto/page_messages.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-persistent-handle.h"

namespace mitto {

class AnimationFrameController;
class CacheStorageHost;
struct ChromeObjectData;
class DOMTimerCoordinator;
class Document;
class DocumentLoadTiming;
class DocumentLoader;
class IdleTaskController;
class PageFetcher;
class HTMLFrameOwnerElement;
class IndexedDbHost;
class PageWorkerStarter;
class ScriptRunner;
class SessionCacheStorage;
class SessionIndexedDb;
class SessionStorageBuckets;
class StorageBucketHost;
class Window;
class WindowTasks;

class PageFrame final : public NavigationClient, public Frame {
 public:
  // A frame of the Page that `runner` runs, whose ID in the Page is `id`: the
  // top-level one, or for a `parent` the child that `owner` holds, named
  // `name`.
  PageFrame(ScriptRunner& runner,
            uint64_t id,
            PageFrame* parent = nullptr,
            HTMLFrameOwnerElement* owner = nullptr,
            std::u16string name = std::u16string());

  PageFrame(const PageFrame&) = delete;
  PageFrame& operator=(const PageFrame&) = delete;

  ~PageFrame() override;

  // The frame's ID in the Page, which it keeps as long as it lives and no
  // other frame of the Page ever has, as Chrome's FrameTreeNode ID.
  uint64_t id() const { return id_; }

  // Makes the frame's first window, whose document is an initial about:blank,
  // as Blink makes a frame's, and whose navigator.connection gives
  // `connection`: a top-level frame's has an opaque origin, as a tab's has,
  // and a child's its parent's document's origin.
  void Start(const PageConnection& connection);

  // The context of the frame's current window, or an empty one before
  // Start().
  v8::Local<v8::Context> context() const;

  // Has the navigation that the Session makes when the Page opens load the
  // frame's document, if it makes one.
  void ExpectOpeningNavigation();

  // Frame:
  Window* DomWindow() const override;
  Frame* Parent() const override;
  size_t ChildCount() const override;
  Frame* ChildAt(size_t index) const override;
  HTMLFrameOwnerElement* Owner() const override;
  const std::u16string& Name() const override;
  void SetName(std::u16string name) override;
  bool IsLoading() const override;
  Frame* CreateChildFrame(HTMLFrameOwnerElement& owner,
                          std::u16string name) override;
  void NavigateFromOwner(const GURL& url) override;
  void Detach() override;

  // NavigationClient:
  void BeginNavigation(const NavigationRequest& request) override;
  void DidNavigateWithinDocument(const GURL& url) override;
  void DidOpenDocument(const Window& opener) override;
  bool HasProvisionalNavigation() const override;
  void StopAllLoaders() override;

  // What a loader of the frame calls.
  //
  // The response of `loader`'s navigation has come: the frame's document
  // stops loading and unloads, and a new context's document at `url` takes
  // its place, which this returns: an XMLDocument of `xml_content_type`
  // where one is given, in a cross-origin isolated agent if
  // `cross_origin_isolated`. `loader` becomes the document's. Null if a
  // listener of the old document's unload events, or of its frames', took
  // the frame away: the response goes, and so does `loader`, from a later
  // task.
  Document* CommitNavigation(
      DocumentLoader& loader,
      const GURL& url,
      base::TimeTicks navigation_start,
      const PageConnection& connection,
      bool cross_origin_isolated,
      const std::optional<std::u16string>& xml_content_type = std::nullopt);
  // `loader`'s navigation ended with no response, with `net_error`: the frame
  // keeps its document, or a child frame commits Chrome's error page for a
  // navigation that failed. `loader` goes.
  void NavigationEndedWithoutDocument(DocumentLoader& loader, int net_error);
  // How the current window's document loads, which its document's loading
  // fills in, and performance.timing and window.chrome read.
  DocumentLoadTiming& document_load_timing();

 private:
  // Takes `loader` from where the frame holds it, as the opening navigation's
  // or the one that waits.
  std::unique_ptr<DocumentLoader> Take(DocumentLoader& loader);

  // Makes a new context, whose window has an empty document at `url`, the
  // response of navigation `navigation`, in place of the current one, tells
  // the Session of it, and returns the document. The old window's timers,
  // idle callbacks, animation frames, fetches, tasks and workers stop with
  // its context. The new window's performance.timeOrigin is
  // `navigation_start`, and its navigator.connection gives `connection`, and
  // its agent is cross-origin isolated if `cross_origin_isolated`. With
  // `xml_content_type`, the document is an XMLDocument of that MIME type, as
  // Blink's DocumentInit makes one for an XML response, and otherwise an
  // HTMLDocument.
  Document& CommitDocument(
      uint64_t navigation,
      const GURL& url,
      base::TimeTicks navigation_start,
      const PageConnection& connection,
      bool cross_origin_isolated,
      const std::optional<std::u16string>& xml_content_type);
  // Makes a window's context, with what Chrome's renderer adds to it, whose
  // time origin is `time_origin` and whose document's origin is `origin`,
  // which decides whether it is a secure context and its security token, in
  // a cross-origin isolated agent if `cross_origin_isolated`. A frame that had
  // a context passes its WindowProxy, the old context's global proxy, which
  // the old context has let go, so that the new context takes it, as
  // LocalWindowProxy reinitializes on the same global proxy.
  v8::Local<v8::Context> NewContext(
      base::TimeTicks time_origin,
      const url::Origin& origin,
      bool cross_origin_isolated = false,
      v8::Local<v8::Object> global_proxy = v8::Local<v8::Object>());
  // The security token of a context of the Page whose document has
  // `origin`, an opaque one, or an empty handle if no frame's document has it.
  v8::Local<v8::Value> TokenOfOpaqueOrigin(const url::Origin& origin) const;
  // Gives the current context's window its timers, what its fetch() goes
  // through, and what it reads of the browser and the machine, with
  // `connection` for its document, and the ID of its document, which it tells
  // the Session of as made of what `committed` says.
  void AttachWindow(const PageConnection& connection,
                    const PageDocumentCommitted& committed);
  // Tells the Session of the frame's new document, as `committed` says what it
  // is made of, and returns its ID, before anything that it sends. What the
  // document before asked of the Session's storage goes with it.
  uint64_t SendDocumentCommitted(PageDocumentCommitted committed);
  // What goes with the current window's context: its timers, idle callbacks,
  // animation frames, fetches, tasks and workers, and the inspector's and
  // window.chrome's hold on it. What runs them stops, and goes with the frame
  // or once the next window's takes its place, as one of them may be what
  // ended the context.
  void DetachWindow();
  // The frame's Cache API, IndexedDB and Storage Buckets: its own for a child
  // frame, the runner's for the top-level frame.
  CacheStorageHost* cache_storage_host() const;
  IndexedDbHost* indexed_db_host() const;
  StorageBucketHost* storage_bucket_host() const;
  // Gives `document`, of an HTTP or HTTPS origin, the frame's storage.
  void GiveStorage(Document& document) const;
  // The frame's children go, each as Detach() has it, when the frame goes or
  // its document goes and its elements with it.
  void DetachChildren();
  // A frame that loaded held its parent's load event back, or an ancestor's,
  // which checks again once the frame stops, as Blink's does: from a task,
  // since the element's removal may still be under way, and not while the
  // parent's own document goes.
  void CheckParentLoadSoon();
  // A srcdoc or an about:blank that waits commits nothing then, and the
  // parent, whose load event it held back, checks again.
  void CancelLocalNavigation();
  // A navigation through the Session that waits for its response ends, as a
  // newer one takes its place.
  void CancelProvisionalNavigation();
  // The window that a frame's initial about:blank leaves to a document of
  // its origin keeps it, as Blink's does, but the Session forgets the old
  // document: its fetches fail, as Chrome's do, and its dedicated workers,
  // which Chrome keeps with the window, are terminated.
  void EndOldDocumentInWindow();
  // The about:blank or data: URL of type 72 that waited ends with no
  // document: the Session forgets what it took of the initiator for it.
  void EndLocalNavigation();
  // Whether two of the frame's ancestors' documents are at `url`, but for the
  // fragment.
  bool IsSelfReferential(const GURL& url) const;
  // The Session records a document that the frame commits without the network
  // at the bare about:blank or about:srcdoc, since message 70 names no URL,
  // so it hears the URL that the document committed at, such as
  // about:blank?v=1, as Chrome's browser knows it.
  void TellOfLocalUrl(const GURL& url);
  // The child frame `child` has gone: this hands it to the runner, which lets
  // go of it once the task ends.
  void RemoveChild(PageFrame& child);
  // RenderFrameImpl::SynchronouslyCommitAboutBlankForBug778318(): the first
  // navigation of a frame's initial about:blank to about:blank, from a
  // document of its origin, commits a new about:blank document at `url` in the
  // same window before it returns, which loads at once, so that its load
  // event and its element's fire then. Chrome keeps the document's token, so
  // the Session knows it by the initial document's ID.
  void CommitAboutBlankSynchronously(const GURL& url);
  // A navigation of the frame that commits without the network: to a srcdoc
  // or an about:blank.
  // A child frame's navigation to a data: URL, which waits for a later task,
  // which a newer navigation takes the place of.
  struct DataUrlAnswer {
    uint64_t number = 0;
    base::TimeTicks start;
  };
  struct LocalNavigation {
    LocalNavigation();
    LocalNavigation(LocalNavigation&&);
    LocalNavigation& operator=(LocalNavigation&&);
    ~LocalNavigation();

    // Its number among the frame's, which the newest takes, and when it
    // started.
    uint64_t number = 0;
    base::TimeTicks start;
    GURL url;
    // Its source, kSrcdoc, kAboutBlank or kErrorPage, and an about:blank's
    // initiator.
    PageDocumentCommitted committed;
    // Its document's origin and referrer policy, the parent document's for a
    // srcdoc, the initiator's for an about:blank, and an opaque origin and
    // the default for an error page, and its referrer, none for an error page.
    url::Origin origin;
    FetchReferrerPolicy referrer_policy = FetchReferrerPolicy::kDefault;
    std::u16string referrer;
    // Its document's about base URL, the parent document's base URL for a
    // srcdoc and the initiator's for an about:blank, and its cookie URL, the
    // parent document's or the initiator's, as Blink's DocumentInit takes it
    // from the owner document.
    GURL about_base_url;
    GURL cookie_url;
    // A srcdoc's markup.
    std::u16string body;
    // Whether it reloads the frame's document, as location.reload() does.
    bool reload = false;
  };
  // Commits `navigation` from a later task, as Chrome's browser commits it,
  // unless a newer navigation of the frame takes its place first.
  void CommitWithoutNetworkSoon(LocalNavigation navigation);
  // A child frame's navigation to a data: URL, which the Page answers itself
  // from a later task, through a DocumentLoader as a response.
  void BeginDataUrlNavigation(const NavigationRequest& request);
  // Gives the loader of the data: URL's navigation numbered `number`, if it
  // still waits, its response.
  void AnswerDataUrl(uint64_t number);
  // Has the parent check whether all of its frames have loaded, once this
  // frame no longer loads.
  void LetParentCheckCompleted();
  // Commits `navigation`, unless another has taken its place: in the same
  // window for a document of the initial about:blank's origin, as Blink's
  // DocumentLoader::CommitNavigation() reuses it, and otherwise in a new
  // context that the frame's WindowProxy takes.
  void CommitWithoutNetwork(uint64_t number);

  const raw_ref<ScriptRunner> runner_;
  const uint64_t id_;
  const raw_ptr<PageFrame> parent_;
  // The element that holds the frame in its parent's document.
  cppgc::WeakPersistent<HTMLFrameOwnerElement> owner_;
  std::u16string name_;
  // The frame's children, in the order they were made.
  std::vector<std::unique_ptr<PageFrame>> children_;
  v8::Global<v8::Context> context_;
  // The window of the frame's current context.
  cppgc::Persistent<Window> window_;
  // Whether a navigation of the frame has committed, or started, since its
  // initial about:blank, which only its first may replace synchronously.
  bool navigated_ = false;
  // Whether Detach() has started, whose unload events may remove the element
  // again, and whether the frame's children and window have started to go
  // after them.
  bool detaching_ = false;
  bool going_ = false;
  // Whether DetachChildren() runs, whose children's going checks nothing of
  // the parent's load.
  bool detaching_children_ = false;
  // What the parser of a synchronously committed about:blank reads: nothing.
  std::unique_ptr<DocumentParser::Client> empty_body_;
  // What the parser of a document that the frame committed without the
  // network reads.
  std::unique_ptr<DocumentParser::Client> local_body_;
  // The number of the last navigation of the frame that commits without the
  // network, and the one that waits to commit, if any, which the newest
  // takes the place of.
  uint64_t last_local_navigation_ = 0;
  std::unique_ptr<LocalNavigation> pending_local_navigation_;
  // The number of the last data: URL that the frame answered itself, and the
  // answer that waits, if any.
  uint64_t last_data_url_answer_ = 0;
  std::unique_ptr<DataUrlAnswer> pending_data_url_answer_;
  // What the current window runs in its context, which stops with it.
  std::unique_ptr<DOMTimerCoordinator> timers_;
  std::unique_ptr<IdleTaskController> idle_tasks_;
  std::unique_ptr<AnimationFrameController> animation_frames_;
  std::unique_ptr<PageFetcher> fetcher_;
  std::unique_ptr<WindowTasks> window_tasks_;
  std::unique_ptr<PageWorkerStarter> worker_starter_;
  // A child frame's own Cache API, IndexedDB and Storage Buckets, which ask
  // the Session as the frame's current document, as each of Blink's frames
  // has remotes of its own. The top-level frame's are the runner's.
  std::unique_ptr<SessionCacheStorage> own_cache_storage_;
  std::unique_ptr<SessionIndexedDb> own_indexed_db_;
  std::unique_ptr<SessionStorageBuckets> own_storage_buckets_;
  // What window.chrome of the current window reads, which the runner keeps
  // as long as the Page, for a callback of a window that has gone.
  raw_ptr<ChromeObjectData> chrome_object_data_ = nullptr;
  // The loader of the frame's document, none for about:blank.
  std::unique_ptr<DocumentLoader> current_;
  // The navigation that the Session makes when the Page opens, until it ends.
  std::unique_ptr<DocumentLoader> opening_;
  // The navigation that a document started, while it waits for its response,
  // and what RenderFrameImpl compares a new one with to drop it as the same.
  std::unique_ptr<DocumentLoader> provisional_;
  NavigationRequest provisional_request_;
  // Counts the requests that have waited, so that a data: URL's navigation
  // lets its request go only while it is still the one that waits.
  uint64_t provisional_request_serial_ = 0;
  base::TimeTicks provisional_start_;
  uint64_t provisional_cookie_writes_ = 0;
  base::WeakPtrFactory<PageFrame> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_PAGE_PAGE_FRAME_H_
