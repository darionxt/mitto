// The library's side of a Page: its process and its end of the channel, on the
// network thread, and the events that wait for whichever thread holds the
// Page's handle.

#ifndef MITTO_PAGE_HOST_H_
#define MITTO_PAGE_HOST_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/circular_deque.h"
#include "base/memory/raw_ref.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "mitto/cross_origin_policies.h"
#include "mitto/network_quality.h"
#include "mitto/page_channel.h"
#include "mitto/page_launcher.h"
#include "mitto/page_messages.h"
#include "mitto/page_storage_buckets.h"
#include "mitto/policy_allowlist.h"
#include "mitto/request_client.h"
#include "mitto/request_options.h"
#include "mitto/storage/storage_key.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_access_result.h"
#include "net/cookies/cookie_change_dispatcher.h"
#include "net/cookies/cookie_partition_key.h"
#include "net/cookies/site_for_cookies.h"
#include "net/http/http_request_headers.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace net {
class CookieStore;
}  // namespace net

namespace mitto {

class CacheStorageContext;
class IndexedDbContext;
class PageCacheStorage;
class PageIndexedDb;
class PageHost;
class PreconnectManager;
class ReadyRequests;
class StorageQuota;
class SessionStorageNamespace;
class StorageContext;

// What a request of one Request Kind sends, as the Session's Persona says: its
// header lines in their order, and its options, with no Initiator, which the
// library sets to the Page's document.
struct PageRequestTemplate {
  PageRequestTemplate();
  PageRequestTemplate(const PageRequestTemplate&);
  PageRequestTemplate& operator=(const PageRequestTemplate&);
  ~PageRequestTemplate();

  net::HttpRequestHeaders headers;
  RequestOptions options;
};

// What mitto_page_open() reads.
struct PageOptions {
  PageOptions();
  PageOptions(PageOptions&&);
  PageOptions& operator=(PageOptions&&);
  ~PageOptions();

  PageStart start;
  // Where the Page navigates when it opens. Without one it stays at
  // about:blank.
  std::optional<GURL> url;
  // When the navigation started: when the caller opened the Page, before its
  // process started, as Chrome's NavigationRequest starts a tab's navigation
  // before its renderer runs. What its document's timeOrigin counts from.
  base::TimeTicks navigation_start;
  // What a request of each Request Kind sends, and for a kind whose requests
  // can have a body, what one with a body sends, whose lines hold the
  // Content-Type where the Persona puts it, which the page's script gives, or
  // which goes if it gives none.
  std::map<PageRequestKind, PageRequestTemplate> requests;
  std::map<PageRequestKind, PageRequestTemplate> requests_with_body;
};

// Returns what mitto_page_open() reads from `json`, or nullopt if it holds an
// unknown key or an invalid value.
std::optional<PageOptions> ParsePageOptions(std::string_view json);

// How many requests a Page may have open at once, as many as Chrome's network
// service allows a renderer process. One that asks for more is refused.
// Upstream: services/network/network_context.h
inline constexpr size_t kMaxPageRequests = 2700;

// The keepalive requests in flight, and the bytes of their URLs and header
// lines, as the network service's KeepaliveStatisticsRecorder counts them, of
// a Session or of one of its Pages. Each request counts until it ends, after
// its Page has gone too.
// Upstream: services/network/keepalive_statistics_recorder.h
struct KeepaliveInFlight final : public base::RefCounted<KeepaliveInFlight> {
  KeepaliveInFlight();

  size_t requests = 0;
  size_t bytes = 0;

 private:
  friend class base::RefCounted<KeepaliveInFlight>;
  ~KeepaliveInFlight();
};

// How many keepalive requests may be in flight at once, of a Session and of
// one Page, which stands for its top-level frame, and how many bytes of URLs
// and header lines one Page's may hold. One past these fails with
// ERR_INSUFFICIENT_RESOURCES.
// Upstream: services/network/url_loader_factory.h
inline constexpr size_t kMaxKeepaliveRequests = 2048;
inline constexpr size_t kMaxKeepaliveRequestsPerPage = 256;
inline constexpr size_t kMaxKeepaliveBytesPerPage = 512 * 1024;

// How many bytes of fetchLater() requests one document, and one Page, may hold
// at once, counted by their URLs without the fragment, their methods,
// referrers, header lines and bodies: three times what Blink's quotas let a
// document queue, no more than the 512 KiB of a top-level control document,
// and a frame tree, those 512 KiB and the 64 KiB that an iframe's allow may
// give each of a page's at most 1,000 frames, since a string body counts there
// by its UTF-16 length and comes as UTF-8, of up to three bytes for each unit.
// One more is dropped.
// Upstream: third_party/blink/renderer/core/fetch/fetch_later_util.h
// Upstream: third_party/blink/renderer/core/page/page.cc
inline constexpr size_t kMaxDeferredBytesPerDocument = 3 * 512 * 1024;
inline constexpr size_t kMaxDeferredBytesPerPage =
    3 * (512 * 1024 + 1000 * 64 * 1024);

// What a Page's Session gives it, on the network thread: its requests and its
// cookies.
class PageNetwork {
 public:
  // Starts a request for a Page, which tells `client` how it goes until it
  // finishes or the client abandons it. It is the Session's, as a request of
  // the C API's is. A body that streams, for `options.stream_body`, is read
  // from `body_channel`, which the client writes.
  virtual base::WeakPtr<RunningRequest> StartPageRequest(
      RequestClient& client,
      GURL url,
      net::HttpRequestHeaders headers,
      RequestOptions options,
      std::optional<std::vector<char>> body,
      scoped_refptr<RequestChannel> body_channel) = 0;

  virtual net::CookieStore& cookie_store() = 0;

  // The keepalive requests of the Session's Pages in flight.
  virtual const scoped_refptr<KeepaliveInFlight>& keepalive_in_flight() = 0;

  // The Session's network estimate and salts, or null for a Session that has
  // none.
  virtual const NetworkQuality* GetNetworkQuality() const = 0;

  // Hands a BroadcastChannel message of a context of the Page `from`, whose
  // storage key is `storage_key`, to each other Page of the Session, as
  // Chrome's browser process hands it to the renderers of other tabs.
  virtual void BroadcastFromPage(const PageHost& from,
                                 const StorageKey& storage_key,
                                 const PageBroadcast& broadcast) = 0;
  // The Session's Cache API, which its Pages share, and the quota of its
  // storage.
  virtual CacheStorageContext& cache_storage() = 0;
  virtual StorageQuota& storage_quota() = 0;
  // The Session's IndexedDB, which its Pages share.
  virtual IndexedDbContext& indexed_db() = 0;

  // The Session's storage, which all its Pages share.
  virtual StorageContext& storage_context() = 0;

  // What resolves and connects as its Pages' documents hint, as Chrome's
  // browser has one PreconnectManager for a profile.
  virtual PreconnectManager& preconnect_manager() = 0;

 protected:
  virtual ~PageNetwork() = default;
};

// How long a Page process has to end once its channel has closed, before it is
// killed. It ends at once when it reads the end of the stream, whatever its
// scripts are doing.
inline constexpr base::TimeDelta kPageExitGrace = base::Seconds(2);

// How many console messages and uncaught errors of a Page wait untaken at most,
// and how many bytes of their text, so that a Page that writes without end
// cannot fill the memory of the process that holds the library. V8's inspector
// keeps as many console messages for DevTools.
inline constexpr size_t kMaxWaitingReports = 1000;
inline constexpr size_t kMaxWaitingReportBytes = 32 * 1024 * 1024;

// One event of a Page. Each MITTO_PAGE_EVENT type uses the fields that
// MittoPageEvent says it does.
struct PageEvent {
  PageEvent();
  PageEvent(PageEvent&&);
  PageEvent& operator=(PageEvent&&);
  ~PageEvent();

  int32_t type = 0;
  int32_t error = 0;
  uint64_t call = 0;
  std::string json;
  std::string message;
  int32_t level = 0;
  int32_t status = 0;
  int32_t net_error = 0;
  std::string url;
  // NAVIGATED: whether the back/forward cache restored the document.
  bool restored = false;
};

// Carries a Page's events from the network thread to whichever thread holds the
// Page's handle. It outlives the Page's Session, so that a call made after the
// Session has gone still gets its event.
class PageEvents : public base::RefCountedThreadSafe<PageEvents> {
 public:
  PageEvents();

  PageEvents(const PageEvents&) = delete;
  PageEvents& operator=(const PageEvents&) = delete;

  // Called on any thread. Returns the number of a new call.
  uint64_t NewCall();

  // Called on any thread.
  //
  // Takes the next event into `event` and returns net::OK. Waits for one for up
  // to `timeout`, or for as long as it takes if that is nullopt. Returns
  // net::ERR_IO_PENDING if none arrived in that time, and at once after
  // EndWaits().
  int NextEvent(std::optional<base::TimeDelta> timeout, PageEvent& event);

  // Called on any thread. Ends each wait of NextEvent(), and makes each later
  // one return at once.
  void EndWaits();

  // The rest is called on the network thread.

  void Push(PageEvent event);

  // Pushes a console message or an uncaught error, unless kMaxWaitingReports of
  // them, or kMaxWaitingReportBytes of their text, wait untaken. Then it drops
  // it, and the next one that has room comes after a console warning that says
  // how many were dropped. Returns how many events it pushed.
  int PushReport(PageEvent event);

  // Pushes the MITTO_PAGE_EVENT_ENDED event, and remembers why, for the calls
  // that come later.
  void PushEnded(int32_t error, std::string message);

  // Pushes the failure of `call`, which came after the Page ended.
  void PushCallAfterEnd(uint64_t call);

 private:
  friend class base::RefCountedThreadSafe<PageEvents>;

  ~PageEvents();

  void AddReport(PageEvent event) EXCLUSIVE_LOCKS_REQUIRED(lock_);

  base::Lock lock_;
  base::ConditionVariable event_waiting_{&lock_};
  base::circular_deque<PageEvent> events_ GUARDED_BY(lock_);
  uint64_t next_call_ GUARDED_BY(lock_) = 1;
  // Why the Page ended, once it has.
  int32_t ended_error_ GUARDED_BY(lock_) = 0;
  std::string ended_message_ GUARDED_BY(lock_);
  bool waits_ended_ GUARDED_BY(lock_) = false;
  // The console messages and uncaught errors among `events_`, and their text.
  size_t waiting_reports_ GUARDED_BY(lock_) = 0;
  size_t waiting_report_bytes_ GUARDED_BY(lock_) = 0;
  // How many were dropped since the last one that had room.
  uint64_t dropped_reports_ GUARDED_BY(lock_) = 0;
};

// One Page, on the network thread, where its Session owns it until the Page
// closes or the Session goes. Its events go into its PageEvents, and each lists
// the Page's handle in the Session's ReadyRequests, beside its requests.
//
// It serves what the Page asks of its Session: requests, which it starts from
// the template of their Request Kind with the Page's document as their
// Initiator, and the document's cookies, as Chrome's network service serves a
// renderer.
class PageHost final : public PageChannel::Delegate {
 public:
  PageHost(scoped_refptr<PageEvents> events,
           scoped_refptr<PageProcess> process,
           ReadyRequests& ready,
           uintptr_t handle,
           PageNetwork& network);

  PageHost(const PageHost&) = delete;
  PageHost& operator=(const PageHost&) = delete;

  // Abandons the Page's requests and closes the channel, which ends the Page
  // process. Whoever closes the Page reaps it.
  ~PageHost() override;

  // Starts to read `ours`, tells the Page how to start, and starts the
  // navigation, if `options` has a URL. Returns false if it cannot read
  // `ours`.
  bool Start(PageChannelEnd ours, PageOptions options);

  void Evaluate(uint64_t call, std::string script);

  // Ends each call in progress and the Page, because its Session is being
  // destroyed, and closes the channel.
  void EndWithSession();

  const scoped_refptr<PageProcess>& process() const { return process_; }

  // Sends the Page a BroadcastChannel message of a context of another Page of
  // the Session, whose storage key is `storage_key`, if the Page has a context
  // of that key.
  void DeliverBroadcast(const StorageKey& storage_key,
                        const PageBroadcast& broadcast);

  base::WeakPtr<PageHost> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

 private:
  class ResponseChecks;
  class Request;

  // PageChannel::Delegate:
  void OnFrame(uint32_t type, std::string payload) override;
  void OnChannelClosed(PageChannel::Closed closed, std::string why) override;

  // A context of the Page's: a document, or a dedicated worker, by the ID
  // that the Page gave it.
  // Where a document's frame is in the frame tree: the top-level document's
  // origin, and whether a document from this one up is of another site, or of
  // another origin, than the top-level one, and for a credentialless document,
  // the nonce of the top-level document's credentialless frames, which
  // partitions its requests, cookies and storage. A worker is in its creator's
  // frame.
  struct FrameTree {
    url::Origin top_frame_origin;
    bool is_top_level = true;
    bool cross_site = false;
    bool cross_origin = false;
    std::optional<base::UnguessableToken> nonce;
  };

  struct Context {
    // The URL that Chrome's browser last committed for the document, which
    // RenderFrameHostImpl::DidOpenDocumentInputStream() leaves where
    // document.open() gives the document another.
    const GURL& LastCommittedUrl() const {
      return url_before_open ? *url_before_open : url;
    }

    // A document's URL, as the library loaded it after its navigation's
    // redirects, or about:blank; a worker's script's final URL, or its blob:
    // or data: URL.
    GURL url;
    // A document's URL as it was before document.open() gave it another,
    // until a navigation within the document commits one.
    std::optional<GURL> url_before_open;
    // A document's origin, its URL's, or for a frame's initial about:blank its
    // parent's; a worker's, its creator's, or an opaque one for data:.
    url::Origin origin;
    // Its place in the frame tree, and what its storage is keyed by, if it has
    // any: CalculateStorageKey()'s. AddContext() works them out.
    FrameTree frame_tree;
    std::optional<StorageKey> storage_key;
    // The document whose frame element holds a document's frame, or the
    // context that started a worker; kNoDocument for the top-level document.
    uint64_t parent = kNoDocument;
    bool is_worker = false;
    // Whether a sandbox makes a document's origin opaque, and with it that of
    // each document under it.
    bool sandboxed_origin = false;
    // Its embedder policy, as the policy container that Chrome's browser
    // keeps for it holds it: a document's from its navigation's response, a
    // worker's from its script's, and a frame's initial about:blank's and a
    // blob: or data: worker's from its parent or creator.
    CrossOriginEmbedderPolicy coep;
    // Whether a document's agent cluster is cross-origin isolated: the
    // top-level document's, which Chrome's browser isolates for each frame of
    // its page (AgentIsCrossOriginIsolated()). The Page has the document's
    // CrossOriginIsolatedCapability(), with its cross-origin-isolated
    // feature, which its permissions policy says.
    bool cross_origin_isolated = false;
    // Whether a document is credentialless, as each under it is, and a
    // worker's creator: its policy container's is_credentialless, fixed as
    // its navigation started.
    bool credentialless = false;
    // For a frame's document: whether its frame's element is credentialless,
    // as the Page last said with message 250, which the frame's next
    // navigation takes, as Chrome's FrameTreeNode keeps it for the frame.
    bool frame_credentialless = false;
    // What the headers of a document's response say of its permissions policy,
    // which PermissionsPolicyHeaders::Allows() answers.
    PermissionsPolicyHeaders policy_headers;
    // The URL whose cookies a document's cookie calls read and write, as
    // Blink's Document::CookieURL(): for a frame's about:blank or srcdoc its
    // creator's or its initiator's, and empty for a document's own URL, as
    // ContextUrl() speaks for it.
    GURL cookie_url;
    // The final URLs of the context's requests that have finished, which a
    // request of the context may name as its referrer, as a module names the
    // module that imports it, or a style sheet the sheet that imports it, in
    // Chrome, whose browser takes the renderer's word for a referrer.
    std::set<GURL> response_urls;
  };

  // What a document's cookie calls are made for, as its frame's
  // RestrictedCookieManager has them: its URL, its frame's site for cookies,
  // and its partition.
  struct CookieContext {
    CookieContext();
    CookieContext(const CookieContext&);
    CookieContext& operator=(const CookieContext&);
    ~CookieContext();

    GURL url;
    net::SiteForCookies site_for_cookies;
    std::optional<net::CookiePartitionKey> partition_key;
  };

  // What the Page asks for. Each of these that comes from a context has what
  // the library loaded it as, never what the Page names.
  void OnRequest(PageRequest asked, uint64_t context_id);
  void OnCredit(const PageCredit& credit);
  void OnGetCookies(uint64_t question, const CookieContext& context);
  void OnSetCookie(const std::string& line, const CookieContext& context);
  void OnCancel(uint64_t number);
  void OnSendDeferredRequest(uint64_t number);
  // A fetch()'s body that streams from the Page, which a request of
  // `options.stream_body` reads.
  void OnRequestBodyPart(const PageRequestBodyPart& part);
  void OnRequestBodyEnded(const PageRequestBodyEnded& ended);
  // The request of `number` whose body streams and has not ended, for `what`
  // of it that the Page sent; null if the request has ended since, or if the
  // Page breaks the protocol, which this refuses.
  Request* RequestWithBodyStream(uint64_t number, std::string_view what);
  // What the document's CookieStore asks, as Chrome's RestrictedCookieManager
  // answers it, always for the document that the Session loaded.
  void OnCookieStoreGet(PageCookieStoreGet get, const CookieContext& context);
  void OnCookieStoreSet(PageCookieStoreSet set, const CookieContext& context);
  void OnCookieStoreListen(bool listen,
                           uint64_t document,
                           const CookieContext& context);
  // A message of one of the document's BroadcastChannels, for the Session's
  // other Pages of the document's origin, which the library takes from the
  // document it loaded, not from the Page.
  void OnBroadcast(const PageBroadcast& broadcast, uint64_t context_id);
  // A document that the Page has made, which takes the place of the
  // top-level one, or of its frame's document, or is a frame's first one.
  void OnDocumentCommitted(const PageDocumentCommitted& committed);
  // What the Page says of the attributes of the frame of a document.
  void OnFrameAttributes(uint64_t document, bool credentialless);
  // A dedicated worker that a context has started.
  void OnWorkerStarted(const PageWorkerStarted& started);
  // A context that goes while its parent stays.
  void OnContextGone(uint64_t context_id);
  // A navigation within document `document` that gave it the URL `url`.
  void OnSameDocumentNavigation(uint64_t document, const std::string& url);
  // A navigation of document `document`'s frame to about:blank that document
  // `initiator` started.
  void OnAboutBlankNavigation(uint64_t document,
                              uint64_t initiator,
                              PageLocalNavigationKind kind);
  // Document `opener` of another window opened document `document`.
  void OnDocumentOpened(uint64_t document, uint64_t opener);
  // A traversal of the session history that leaves document `document` for
  // an entry of another document: the navigation that made the record's
  // document again, from what the library kept of it, never from what the
  // Page names, and taking what the cache holds, as Chrome's browser repeats
  // an entry's navigation.
  void OnTraversalRequest(const PageTraversalRequest& traversal,
                          uint64_t document);
  // Records that no entry of the session history holds any more.
  void OnRecordsReleased(const std::vector<uint64_t>& records);
  // The Page's back/forward cache keeps the top-level document `document`,
  // whose context the library keeps apart, though another takes its place.
  void OnDocumentStored(uint64_t document);
  // The Page restored the stored document `document`, as the library asked,
  // which takes the place of the top-level document and of everything under
  // it.
  void OnDocumentRestored(uint64_t document);
  // The stored document `document` goes.
  void OnDocumentEvicted(uint64_t document);
  // Top-level document `id`, of `context`, takes the place of the one before
  // it and of everything under it, whose messages all came before this.
  void ReplaceTopLevelDocument(uint64_t id, Context context);
  // Pushes MITTO_PAGE_EVENT_NAVIGATED for a navigation that ended at `url`
  // with `net_error`, and `status` for one that kept the document, or that
  // the back/forward cache `restored`.
  void PushNavigated(std::string url,
                     int32_t status,
                     int32_t net_error,
                     bool restored = false);
  // Context `id`, a stored document's included, or null.
  Context* FindContext(uint64_t id);
  // Context `id`, or null after refusing a message of `type` that names a
  // context that is not the Page's. A message of a document that the
  // back/forward cache keeps goes unheard.
  const Context* ContextOf(uint64_t id, uint32_t type);
  // The URL that what a context asks for is judged by: its own, or its
  // origin's for a frame's initial about:blank or a worker of a blob: URL.
  static GURL ContextUrl(const Context& context);
  // Context `id` and each context under it: the documents of its frames, the
  // workers it started, and theirs.
  std::set<uint64_t> ContextsUnder(uint64_t id) const;
  // Forgets `ids`, which go, with what waits for them, and lets their
  // keepalive requests go on without them.
  void ForgetContexts(const std::set<uint64_t>& ids);
  // Only a document's keepalive request goes on without it in Chrome, in the
  // browser's KeepAliveURLLoaderService. The network service ends a worker's
  // once its thread has ended.
  bool IsOfWorker(const Request& request) const;
  // Lets each keepalive request of `ids`, or of every context for null, go on
  // without the Page, which hears that it ended, but a worker's, whose requests
  // all end.
  void LetKeepaliveRequestsGo(const std::set<uint64_t>* ids);
  // What a request of context `id` goes as, beside its Kind's: its
  // Initiator, its referrer's source, and its frame's place in the frame
  // tree. For a frame's navigation, `parent` is the frame's parent, from
  // which the tree counts.
  void FillContextFetch(uint64_t id, uint64_t parent, FetchParams& fetch) const;
  // The URL of the Page's top-level document, or an empty one before the Page
  // has made one.
  GURL TopDocumentUrl() const;
  // What the cookie calls of document `id` are made for.
  CookieContext CookieContextOf(uint64_t id) const;
  // The cookie URL of a context, which a frame's document that takes the
  // place of its creator's or initiator's takes, as Blink's
  // DocumentInit::GetCookieUrl() does from its owner document.
  static GURL CookieUrlOf(const Context& context);
  // Whether the agent cluster of a frame's document under document `parent` is
  // cross-origin isolated: its page's, whatever its origin.
  bool AgentIsCrossOriginIsolated(uint64_t parent) const;
  // The embedder policy of a frame's document under document `parent` that
  // the response that `event` describes makes.
  CrossOriginEmbedderPolicy FrameEmbedderPolicyOf(
      uint64_t parent,
      const RequestEvent& event) const;
  // Whether a frame's navigation, whose response that `event` describes makes
  // a document of embedder policy `coep`, is blocked by its parent's.
  static bool BlocksFrameNavigation(const Request& request,
                                    const RequestEvent& event,
                                    const CrossOriginEmbedderPolicy& coep);
  // Ends a frame's navigation that its parent's embedder policy blocks.
  void BlockFrameNavigation(Request& request);
  // What the document's Cache API asks, which PageCacheStorage serves,
  // with the storage key of the document that asks.
  void OnCacheStorage(PageCacheStorageCall call,
                      uint64_t context_id,
                      const GURL& document_url);
  // What a context's IndexedDB asks, which PageIndexedDb serves, with the
  // storage key of the context that asks.
  void OnIndexedDb(PageIndexedDbCall call,
                   uint64_t context_id,
                   const GURL& document_url);
  // What a context's Storage Buckets API asks, which PageStorageBuckets
  // serves, with the storage key of the context that asks.
  void OnStorageBuckets(PageStorageBucketCall call,
                        uint64_t context_id,
                        const GURL& document_url);
  // The storage key of context `context_id`'s IndexedDB and Cache API, as
  // ContextSerializedStorageKey() gives it, if the context is a secure one,
  // as the Cache API and the Storage Buckets API need.
  std::optional<std::string> SecureContextStorageKey(uint64_t context_id,
                                                     const GURL& url);
  // Where a call that names bucket `bucket` of context `context_id` goes, of
  // the storage key `storage_key`: its default bucket for 0, made if it has
  // none, as Chrome makes it once IndexedDB or the Cache API reaches it.
  // None, after the Page has been refused, for a bucket that it was not
  // given.
  std::optional<PageStorageBuckets::Target> LocateBucket(
      uint64_t bucket,
      uint64_t context_id,
      const std::string& storage_key);
  // What a document's localStorage and sessionStorage ask, as Chrome's
  // storage service answers a renderer: an area takes its storage key from the
  // URL that the library loaded the document at that binds it.
  void OnStorageBind(const PageStorageBind& bind, uint64_t document);
  void OnStorageGetAll(const PageStorageGetAll& get_all);
  void OnStorageMutation(PageMessage type, PageStorageMutation mutation);
  void OnStorageUnbind(uint64_t area);
  // A connection that document `document` hints it will want, as Chrome's
  // NetworkHintsHandlerImpl hears of it, with the network anonymization key
  // of the document's subresources.
  void OnNetworkHint(const PageNetworkHint& hint, uint64_t document);

  // What the library checks of a Page's request as it goes, each hop and
  // redirect included.
  struct FetchChecks {
    // Mode cors, a fetch's or an element's, whose responses from another
    // origin pass the CORS check.
    bool cors = false;
    // Mode same-origin: no hop may go to another origin.
    bool same_origin = false;
    // Mode no-cors: a response from another origin is opaque, and needs no
    // CORS check.
    bool no_cors = false;
    // Credentials mode include, whose CORS check wants the request's own
    // origin and Access-Control-Allow-Credentials.
    bool credentials = false;
    // A request that outlives its document and its Page.
    bool keepalive = false;
    // A navigation, whose response the Page makes a document of, unless it
    // is a 204, a 205 or a download.
    bool is_navigation = false;
    // A navigation of the top-level frame, which the caller hears of, and
    // whose COOP and COEP make the Page's agent cluster.
    bool top_level = false;
    // A dedicated worker's script: its creator's embedder policy, which the
    // worker's has to be compatible with, as DedicatedWorkerHost::CheckCOEP()
    // holds it.
    std::optional<CrossOriginEmbedderPolicy> creator_embedder_policy;
    // The embedder policy of the document or worker that asks, which the
    // Cross-Origin-Resource-Policy check of a request of mode no-cors reads.
    CrossOriginEmbedderPolicy embedder_policy;
    // A frame's navigation: the URLs of the frame's ancestors' documents, from
    // its parent's up, as they were when it started.
    std::vector<GURL> frame_ancestor_urls;
    // A frame's navigation: its parent's embedder policy and origin, which
    // NavigationRequest::EnforceCOEP() and CheckResponseAdherenceToCoep()
    // hold each of its responses to.
    std::optional<CrossOriginEmbedderPolicy> parent_embedder_policy;
    url::Origin parent_origin;
    // A frame's navigation: the URL that Chrome's browser last committed for
    // its parent's document as it started, which
    // CheckCredentialedSubresource() holds each redirect to.
    std::optional<GURL> parent_committed_url;
  };

  // Starts a request numbered `number`, whose client is a Request that
  // `requests_` holds, and which goes on only as `checks` allow, for context
  // `context_id` of `document_origin`, opaque for a navigation.
  void StartRequest(uint64_t number,
                    uint64_t context_id,
                    const url::Origin& document_origin,
                    GURL url,
                    net::HttpRequestHeaders headers,
                    RequestOptions options,
                    std::optional<std::vector<char>> body,
                    scoped_refptr<RequestChannel> body_channel,
                    FetchChecks checks);

  // fetchLater()'s request, which waits until the Page asks for it, its
  // document goes or the Page ends, as Chrome's FetchLaterLoader waits in the
  // browser, with what StartRequest() then takes.
  struct DeferredRequest {
    DeferredRequest();
    DeferredRequest(DeferredRequest&&);
    DeferredRequest& operator=(DeferredRequest&&);
    ~DeferredRequest();

    uint64_t context_id = 0;
    url::Origin document_origin;
    GURL url;
    net::HttpRequestHeaders headers;
    RequestOptions options;
    std::optional<std::vector<char>> body;
    FetchChecks checks;
    // What it holds against kMaxDeferredBytesPerPage.
    size_t bytes = 0;
  };
  // Whether one more keepalive request of `url` and `headers` is past what
  // the network service allows in flight, of the Session or of the Page.
  bool IsPastKeepaliveLimits(const GURL& url,
                             const net::HttpRequestHeaders& headers) const;
  // Starts the deferred request numbered `number`, if one waits, and lets it
  // go on without the Page, which hears nothing of it, as the browser answers
  // a FetchLaterLoader's request to nobody.
  void SendDeferredRequest(uint64_t number);
  // Sends the deferred requests of the contexts `ids`, or of every context.
  void SendDeferredRequests(const std::set<uint64_t>* ids);

  // What a Request hears of its job.
  void OnResponseStarted(Request& request, RequestEvent event);
  void OnBodyRead(Request& request, size_t size);
  // A navigation whose response the Page makes no document of: it keeps its
  // document, and the library reads the body to its end and drops it.
  void DropNavigation(Request& request, const RequestEvent& event);
  // Tells the Page that the response of `request` has started.
  // Returns whether the response reached the Page: a frame's navigation that
  // its parent's embedder policy blocks ended instead.
  bool StartResponse(Request& request, const RequestEvent& event);
  // Ends the sniffing of a navigation's response: a download, or the
  // response with what was read of its body.
  void FinishSniffing(Request& request);
  // This deletes `request`.
  void OnRequestFinished(Request& request, int net_error);
  // Reads more of the body of `request` if the Page has given credit for it.
  void ReadIfCredited(Request& request);

  void SendCookies(uint64_t question,
                   const net::CookieAccessResultList& cookies,
                   const net::CookieAccessResultList& excluded);
  void SendCookieStoreCookies(const PageCookieStoreGet& get,
                              const net::CookieAccessResultList& cookies,
                              const net::CookieAccessResultList& excluded);
  void OnCookieStoreChange(uint64_t document,
                           const net::CookieChangeInfo& change);

  // Whether a document at `document_url` can have cookies, which only one that
  // the Session loaded over HTTP or HTTPS can.
  static bool DocumentHasCookies(const GURL& document_url);
  // The partition of the cookies of a frame of `frame_origin` under a
  // top-level frame of `top_frame_origin`, with `site_for_cookies`, as
  // Chrome's RestrictedCookieManager has it, and `nonce` for a credentialless
  // document's.
  static std::optional<net::CookiePartitionKey> FramePartitionKey(
      const url::Origin& top_frame_origin,
      const url::Origin& frame_origin,
      const net::SiteForCookies& site_for_cookies,
      const std::optional<base::UnguessableToken>& nonce);
  // Keeps `context`, a document or a worker that the Page has made, as `id`.
  void AddContext(uint64_t id, Context context);
  // The top-level document over context `id`, or `id` itself.
  uint64_t TopDocumentOf(uint64_t id) const;
  // The nonce of the credentialless frames under top-level document `top`,
  // made at the first, as Chrome's PageImpl keeps one for each main document.
  const base::UnguessableToken& CredentiallessNonce(uint64_t top);

  // The storage key of `context`, as Chrome works it out for a frame with
  // third-party storage partitioning, which it turns on: the document's
  // origin, the site of the top-level document and whether a document from
  // this one up is of another site. None for a document of an opaque origin, or
  // one that is not of an HTTP or HTTPS URL, which have no storage, and for a
  // document under a top-level one of an opaque origin, or a credentialless
  // one, whose key no other top-level document ever has.
  static std::optional<StorageKey> CalculateStorageKey(const Context& context);

  // The storage key of context `id`'s IndexedDB and Cache API, as
  // StorageKey::Serialize() writes it: CalculateStorageKey()'s, or for a
  // credentialless context one of its frame tree's nonce, or for a context
  // under a top-level document of an opaque origin one of that document's own,
  // which the Session keeps in memory. None for a context of an opaque origin,
  // or one that is not of an HTTP or HTTPS URL.
  std::optional<std::string> ContextSerializedStorageKey(uint64_t id);

  // Lets go of every storage area that the Page bound.
  void ReleaseStorage();

  // What navigator.connection gives a document at `url`: what the renderer
  // process of its site gives for its host, as for a subresource to it.
  PageConnection ConnectionOf(const GURL& url) const;

  // Abandons each request in progress.
  void AbandonRequests();
  // The requests in flight: the Page's, and those of its gone workers that
  // //net has yet to send, which still count, as their loaders would in
  // Chrome's network service until the worker's end reached it.
  size_t RequestsInFlight();

  // Refuses what the Page sent: tells it why, closes the channel and kills the
  // process, because a Page that sends what it should not may be hostile.
  void Refuse(std::string why);

  // Closes the channel, and finds out how the process ended, without blocking
  // the network thread.
  void WaitForExit();
  void OnExited(std::string how);

  // Fails each call in progress, and then pushes MITTO_PAGE_EVENT_ENDED.
  void End(int32_t error, std::string message);

  void Push(PageEvent event);
  void PushReport(PageEvent event);
  // The warning on a frame's parent's console that its navigation to a URL
  // with credentials is blocked.
  void WarnOfCredentialedSubresource();

  const scoped_refptr<PageEvents> events_;
  const scoped_refptr<PageProcess> process_;
  const raw_ref<ReadyRequests> ready_;
  const uintptr_t handle_;
  const raw_ref<PageNetwork> network_;
  std::unique_ptr<PageChannel> channel_;
  PageOptions options_;
  // The Page's contexts, its documents and dedicated workers, by the ID that
  // the Page gave each: the Initiator of their requests, and whose cookies a
  // document's document.cookie reads and writes. A Page has one top-level
  // document once it has told the library of it, and the documents of its
  // frames and its workers under it.
  std::map<uint64_t, Context> contexts_;
  // The largest ID the Page has given a context.
  uint64_t last_context_id_ = kNoDocument;
  // The navigations of frames whose responses the Page has and has not made a
  // document of yet, by number: the final URL, the frame's parent and its
  // document that the new one replaces.
  struct FrameNavigationResponse {
    GURL url;
    uint64_t parent = kNoDocument;
    uint64_t target = kNoDocument;
    // The embedder policy and the isolation of the document that the response
    // makes.
    CrossOriginEmbedderPolicy coep;
    bool cross_origin_isolated = false;
    // Whether the document is credentialless, as the navigation started.
    bool credentialless = false;
    PermissionsPolicyHeaders policy_headers;
  };
  std::map<uint64_t, FrameNavigationResponse> frame_navigation_responses_;
  // The navigations of frames to about:blank that have started and not
  // committed yet, by the frame's document that the about:blank takes the
  // place of: the document that started it, and what of it the about:blank
  // takes.
  struct AboutBlankNavigation {
    PageLocalNavigationKind kind = PageLocalNavigationKind::kAboutBlank;
    uint64_t initiator = kNoDocument;
    url::Origin origin;
    CrossOriginEmbedderPolicy coep;
    GURL cookie_url;
    // Whether its document is credentialless, as it started.
    bool credentialless = false;
  };
  std::map<uint64_t, AboutBlankNavigation> about_blank_navigations_;
  // The responses of the scripts of workers that the Page has and has not
  // started yet, by number: the context that asked and the final URL.
  struct WorkerScript {
    uint64_t creator = kNoDocument;
    GURL url;
    // The embedder policy that the response gives the worker.
    CrossOriginEmbedderPolicy coep;
  };
  std::map<uint64_t, WorkerScript> worker_scripts_;
  // The navigations whose responses the Page has and has not made a document
  // of yet, by number: the final URL, the status, the embedder policy that
  // it gives the document, and how many of its responses had a COOP that
  // Chrome's browser ignored, saying so on the document's console.
  struct NavigationResponse {
    GURL url;
    int32_t status = 0;
    CrossOriginEmbedderPolicy coep;
    bool cross_origin_isolated = false;
    int coop_ignored = 0;
    // What the navigation went as, and the Referer it sent last, which a
    // traversal to its document repeats.
    FetchParams fetch;
    std::string referrer;
    // Whether its Cache-Control says no-store.
    bool no_store = false;
    PermissionsPolicyHeaders policy_headers;
  };
  std::map<uint64_t, NavigationResponse> navigation_responses_;
  // The top-level documents that the Page's back/forward cache keeps, by ID:
  // each one's context, apart from the Page's, and its response's status.
  struct StoredDocument {
    Context context;
    int32_t status = 0;
  };
  std::map<uint64_t, StoredDocument> stored_documents_;
  // The status of the top-level document's response, which a stored
  // document keeps.
  int32_t top_level_status_ = 0;
  // The traversal to the entry of a stored document that the library may
  // restore, until the Page restores the document, or the library makes the
  // request after all, or the Page lets it go.
  struct PendingRestore {
    uint64_t number = 0;
    uint64_t document = kNoDocument;
  };
  std::optional<PendingRestore> pending_restore_;
  // For each top-level document of a no-store response, the current one or a
  // stored one, by ID: whether a cookie of its URL has changed since it
  // committed, which keeps it from being restored, as Chrome's
  // RenderFrameHostImpl::CookieChangeListener counts.
  struct NoStoreWatch {
    NoStoreWatch();
    NoStoreWatch(NoStoreWatch&&);
    NoStoreWatch& operator=(NoStoreWatch&&);
    ~NoStoreWatch();

    std::unique_ptr<net::CookieChangeSubscription> subscription;
    bool cookie_changed = false;
  };
  std::map<uint64_t, NoStoreWatch> no_store_watches_;
  // What the library kept of the navigation that made each document that an
  // entry of the Page's session history may go back to, by the document's
  // ID, as Chrome's FrameNavigationEntry keeps it: what the navigation went
  // as, its URL after its redirects, the Referer it sent last, and the URLs
  // that navigations within the document gave it, the most recent last, with
  // how many bytes they hold.
  struct NavigationRecord {
    NavigationRecord();
    NavigationRecord(NavigationRecord&&);
    NavigationRecord& operator=(NavigationRecord&&);
    ~NavigationRecord();

    FetchParams fetch;
    GURL url;
    std::string referrer;
    std::vector<GURL> same_document_urls;
    size_t same_document_url_bytes = 0;
    // Whether document.open() gave the document about:blank, or a traversal
    // took it there, so that an entry of its may be at it.
    bool had_about_blank = false;
  };
  std::map<uint64_t, NavigationRecord> records_;
  // Whether an entry of the top-level document `context` may be at about:blank:
  // a document of its origin whose record the Page still holds had it.
  bool MayHaveAboutBlankEntry(const Context& context) const;
  // The documents whose CookieStores listen, by ID: what tells each of each
  // change, and what its cookie calls are made for.
  struct CookieStoreListener {
    CookieStoreListener();
    CookieStoreListener(CookieStoreListener&&);
    CookieStoreListener& operator=(CookieStoreListener&&);
    ~CookieStoreListener();

    std::unique_ptr<net::CookieChangeSubscription> subscription;
    CookieContext context;
  };
  std::map<uint64_t, CookieStoreListener> cookie_store_listeners_;
  // The storage areas that the Page has bound, by number, the largest number
  // it has used, and its sessionStorage, which lasts as long as the Page.
  class StorageArea;
  std::map<uint64_t, std::unique_ptr<StorageArea>> storage_areas_;
  uint64_t last_storage_area_ = 0;
  uint64_t last_storage_observer_ = 0;
  std::unique_ptr<SessionStorageNamespace> session_storage_;
  // The localStorage and sessionStorage of the frames under a top-level
  // document of an opaque origin, by that document's ID, which stay in memory
  // for as long as the Page.
  std::map<std::pair<uint64_t, PageStorageKind>,
           std::unique_ptr<SessionStorageNamespace>>
      opaque_top_storage_;
  // What stands for the opaque origin of each top-level document whose frames
  // have asked for IndexedDB or the Cache API, by that document's ID.
  std::map<uint64_t, base::UnguessableToken> opaque_top_nonces_;
  // The nonce of each top-level document whose frames are credentialless, by
  // that document's ID, and their localStorage and sessionStorage, by the
  // nonce, which stay in memory for as long as the Page.
  std::map<uint64_t, base::UnguessableToken> credentialless_nonces_;
  std::map<std::pair<base::UnguessableToken, PageStorageKind>,
           std::unique_ptr<SessionStorageNamespace>>
      credentialless_storage_;
  // The requests in progress, by number, and the largest number the Page has
  // used.
  std::map<uint64_t, std::unique_ptr<Request>> requests_;
  // The jobs of gone workers' requests that //net has yet to send, which end
  // once it has, or with the Page.
  std::vector<base::WeakPtr<RunningRequest>> sending_jobs_;
  uint64_t last_request_number_ = kNavigationRequest;
  // The deferred requests that wait, by number.
  std::map<uint64_t, DeferredRequest> deferred_requests_;
  // The bytes that they hold, in all and by the ID of the context that asked.
  size_t deferred_bytes_ = 0;
  std::map<uint64_t, size_t> deferred_bytes_of_context_;
  // Takes a deferred request's bytes off what they hold.
  void ForgetDeferredBytes(const DeferredRequest& deferred);
  // The Page's keepalive requests in flight, with those that have outlived
  // their documents.
  const scoped_refptr<KeepaliveInFlight> keepalive_in_flight_ =
      base::MakeRefCounted<KeepaliveInFlight>();
  bool hello_received_ = false;
  // Why the Page refused what the library sent, if it did.
  std::optional<std::string> refused_by_page_;
  // Why the library refused what the Page sent, if it did.
  std::optional<std::string> refused_by_library_;
  bool waiting_for_exit_ = false;
  bool ended_ = false;
  // The calls in progress, which the Page answers in any order.
  std::set<uint64_t> calls_;
  // The document's Cache API, from its first call.
  std::unique_ptr<PageCacheStorage> cache_storage_;
  // The first number that the next document's Cache API gives a cache or a
  // stored body, past those of the documents before.
  uint64_t next_cache_storage_number_ = 1;
  // The document's IndexedDB, from its first call until the Page goes.
  std::unique_ptr<PageIndexedDb> indexed_db_;
  // The Storage Buckets API of the Page's contexts, from its first call until
  // the Page goes.
  std::unique_ptr<PageStorageBuckets> storage_buckets_;
  base::WeakPtrFactory<PageHost> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_PAGE_HOST_H_
