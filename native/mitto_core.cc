#include <stdint.h>
#include <stdlib.h>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/at_exit.h"
#include "base/barrier_closure.h"
#include "base/byte_size.h"
#include "base/check.h"
#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/containers/unique_ptr_adapters.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/memory/raw_ref.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/message_loop/message_pump_type.h"
#include "base/no_destructor.h"
#include "base/notreached.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/types/optional_util.h"
#include "build/build_config.h"
#include "mitto/body_stream.h"
#include "mitto/client_hints.h"
#include "mitto/connect_retrying_socket_factory.h"
#include "mitto/cookie_json.h"
#include "mitto/directory_lock.h"
#include "mitto/feature_list.h"
#include "mitto/fetch_rules.h"
#include "mitto/ignore_errors_cert_verifier.h"
#include "mitto/mitto.h"
#include "mitto/network_quality.h"
#include "mitto/network_session_configurator.h"
#include "mitto/ready_requests.h"
#include "mitto/request_channel.h"
#include "mitto/request_options.h"
#include "mitto/session_settings.h"
#include "mitto/socks5_auth_socket_factory.h"
#include "mitto/ssl_config_service_mojo.h"
#include "mitto/stub_resolver_config_reader.h"
#include "net/base/auth.h"
#include "net/base/completion_once_callback.h"
#include "net/base/elements_upload_data_stream.h"
#include "net/base/io_buffer.h"
#include "net/base/net_errors.h"
#include "net/base/network_anonymization_key.h"
#include "net/base/network_change_notifier.h"
#include "net/base/network_handle.h"
#include "net/base/request_priority.h"
#include "net/base/transport_info.h"
#include "net/base/upload_bytes_element_reader.h"
#include "net/cert/caching_cert_verifier.h"
#include "net/cert/cert_net_fetcher.h"
#include "net/cert/cert_verifier.h"
#include "net/cert/cert_verify_proc.h"
#include "net/cert/coalescing_cert_verifier.h"
#include "net/cert_net/cert_net_fetcher_url_request.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_access_result.h"
#include "net/cookies/cookie_options.h"
#include "net/cookies/cookie_store.h"
// //net/dns keeps these three headers to the targets on its friend lists, to
// track who uses them outside the network stack. //mitto uses them as
// //services/network does.
#include "net/dns/host_resolver.h"                      // nogncheck
#include "net/dns/host_resolver_manager.h"              // nogncheck
#include "net/dns/system_dns_config_change_notifier.h"  // nogncheck
#include "net/http/http_cache.h"
#include "net/http/http_connection_info.h"
#include "net/http/http_network_session.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_response_info.h"
#include "net/http/http_status_code.h"
#include "net/http/http_transaction_factory.h"
#include "net/http/http_util.h"
#include "net/log/net_log.h"
#include "net/proxy_resolution/proxy_config_service_fixed.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"
#include "net/quic/quic_context.h"
#include "net/socket/client_socket_factory.h"
#include "net/ssl/ssl_info.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/redirect_info.h"
#include "net/url_request/referrer_policy.h"
#include "net/url_request/url_request.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_builder.h"
#include "url/gurl.h"
#include "url/origin.h"

#if BUILDFLAG(IS_WIN)
#include "mitto/pin_library.h"
#endif

static_assert(MITTO_PRIORITY_THROTTLED == net::THROTTLED);
static_assert(MITTO_PRIORITY_IDLE == net::IDLE);
static_assert(MITTO_PRIORITY_LOWEST == net::LOWEST);
static_assert(MITTO_PRIORITY_LOW == net::LOW);
static_assert(MITTO_PRIORITY_MEDIUM == net::MEDIUM);
static_assert(MITTO_PRIORITY_HIGHEST == net::HIGHEST);

namespace mitto {
class RequestJob;
}  // namespace mitto

// The handle the C API hands out for a Session. It is created, used and
// destroyed only on the network thread, except for `ready_requests`. Other
// threads just hold the pointer.
struct MittoSession {
  MittoSession();
  ~MittoSession();

  // Ends each running request, with its last event.
  void CancelJobs();

  // Says that this Session has its directory for the HTTP cache, if any. It
  // comes first, so that it goes after everything that writes there.
  mitto::DirectoryLock http_cache_directory_lock;
  // The resolver of a Session with a DNS server of its own, which the others
  // must not ask. `context` points to it, so it has to come first.
  std::unique_ptr<net::HostResolverManager> own_host_resolver_manager;
  std::unique_ptr<net::URLRequestContext> context;
  // The verifier in `context` holds this, and this makes its requests in
  // `context`, until the destructor shuts it down.
  scoped_refptr<net::CertNetFetcherURLRequest> cert_net_fetcher;
  std::unique_ptr<net::HostResolver::ProbeRequest> doh_probes_request;
  base::OneShotTimer doh_probes_timer;
  // What to answer the Session's proxy with when it asks.
  std::optional<net::AuthCredentials> proxy_credentials;
  mitto::ClientHintsStore client_hints_store;
  // What rtt and downlink are worked out from, if anything.
  std::optional<mitto::NetworkQuality> network_quality;
  // Stands for what //net reads from a server's ALPS. For tests only.
  std::map<url::Origin, std::string> accept_ch_via_alps;
  // After `context`, because a request has to go before its context does.
  std::set<std::unique_ptr<mitto::RequestJob>, base::UniquePtrComparator> jobs;
  mitto::ReadyRequests ready_requests;
};

// The handle the C API hands out for a request. Whichever thread holds it talks
// to the request through `channel`, and has the network thread act on `job`,
// which goes away when the request ends.
struct MittoRequest {
  MittoRequest();
  ~MittoRequest();

  scoped_refptr<mitto::RequestChannel> channel;
  base::WeakPtr<mitto::RequestJob> job;
};

MittoRequest::MittoRequest() = default;
MittoRequest::~MittoRequest() = default;

namespace mitto {
namespace {

// How long a Session waits to start DoH probes after the process has created
// the network core: the default of kDelayInitialDohProbeTimeoutParam, which
// lives outside //net.
constexpr base::TimeDelta kDelayInitialDohProbeTimeout = base::Seconds(5);

// How long the first Session waits for the system's DNS settings. Reading them
// takes milliseconds, and a machine where it fails never answers.
constexpr base::TimeDelta kSystemDnsConfigTimeout = base::Seconds(1);

// The subdirectory of a Session's cache directory that //net owns, with the name
// that PopulateHttpCacheDirectories() in //content gives it.
constexpr base::FilePath::CharType kCacheDataDirectoryName[] =
    FILE_PATH_LITERAL("Cache_Data");

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation(
        "mitto_request",
        "A request made by the application that loaded the library.");

constexpr net::NetworkTrafficAnnotationTag kProxyTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation(
        "mitto_proxy",
        "The proxy, if any, that the application named for a Session.");

std::string HeaderLines(const net::HttpResponseHeaders& headers) {
  std::string lines;
  size_t iterator = 0;
  std::string name;
  std::string value;
  while (headers.EnumerateHeaderLines(&iterator, &name, &value)) {
    base::StrAppend(&lines, {name, ": ", value, "\r\n"});
  }
  return lines;
}

}  // namespace

// One request on the network thread. It is made on the thread that starts the
// request, and after that lives only on the network thread, where its Session
// owns it until the request ends. It never waits for the thread that holds the
// request's handle, and what it has to say goes through the channel.
class RequestJob final : public net::URLRequest::Delegate,
                         public BodyStream::Delegate {
 public:
  // `handle` names the request in the Session's ReadyRequests.
  RequestJob(MittoSession& session,
             uintptr_t handle,
             scoped_refptr<RequestChannel> channel,
             GURL url,
             net::HttpRequestHeaders headers,
             RequestOptions options,
             std::optional<std::vector<char>> body)
      : session_(session),
        handle_(handle),
        channel_(std::move(channel)),
        url_(std::move(url)),
        request_headers_(std::move(headers)),
        options_(std::move(options)),
        body_(std::move(body)) {
    if (options_.fetch) {
      fetch_rules_.emplace(*std::move(options_.fetch), url_,
                           session_->client_hints_store,
                           base::OptionalToPtr(session_->network_quality));
    }
  }

  RequestJob(const RequestJob&) = delete;
  RequestJob& operator=(const RequestJob&) = delete;

  ~RequestJob() override = default;

  // May be called on any thread until the first time the pointer is used.
  base::WeakPtr<RequestJob> GetWeakPtr() {
    return weak_ptr_factory_.GetWeakPtr();
  }

  void Start() {
    request_ = CreateRequest(url_, options_.method, request_headers_);
    if (fetch_rules_) {
      fetch_rules_->ConfigureRequest(*request_);
    } else {
      SetUpTopLevelNavigation(*request_);
    }
    SetCallersReferrer();
    if (body_) {
      SetBody();
    }
    if (options_.stream_body) {
      auto body_stream = std::make_unique<BodyStream>(
          channel_, options_.stream_body_size, *this);
      body_stream_ = body_stream->GetWeakPtr();
      request_->set_upload(std::move(body_stream));
    }
    if (options_.total_timeout) {
      total_timer_.Start(FROM_HERE, *options_.total_timeout,
                         base::BindOnce(&RequestJob::Finish,
                                        base::Unretained(this),
                                        net::ERR_TIMED_OUT));
    }
    StartConnectTimer();
    request_->Start();
  }

  // Reads the next part of the body, as the caller has asked. The result goes to
  // OnReadCompleted(), and no more is read until the caller asks again.
  void ReadBody() {
    if (!response_started_) {
      read_wanted_ = true;
      return;
    }
    const int result =
        request_->Read(channel_->read_buffer(), RequestChannel::kReadBufferSize);
    if (result == net::ERR_IO_PENDING) {
      StartReadTimer();
      return;
    }
    OnReadCompleted(request_.get(), result);
  }

  // Goes on with a read of the stream body that waits for the caller, who has now
  // answered. This may delete the job, so nothing may touch it afterwards.
  void SendBody() {
    // A 303 drops the body, and with it the stream.
    if (body_stream_) {
      body_stream_->OnCallerAnswered();
    }
  }

  void Cancel() { Finish(net::ERR_ABORTED); }

  // BodyStream::Delegate:
  void OnBodyWanted() override { session_->ready_requests.Add(handle_); }

  // A wait for the caller is not a wait for the server. //net reads no response
  // until it has the whole body, so nothing else needs this timer in that time.
  void OnWaitForCallerStarted() override { read_timer_.Stop(); }
  void OnWaitForCallerEnded() override { StartReadTimer(); }

  // net::URLRequest::Delegate:
  int OnConnected(net::URLRequest* request,
                  const net::TransportInfo& info,
                  net::CompletionOnceCallback callback) override {
    connect_timer_.Stop();
    StartReadTimer();
    // A stream cannot go twice, so a request with one goes on as it is.
    if (!fetch_rules_ || options_.stream_body) {
      return net::OK;
    }

    // Nothing has gone out on this request yet, so going on from a later task
    // shows nothing on the wire:
    // NavigationURLLoaderImpl::OnAcceptCHFrameReceived() answers from another
    // process too.
    std::optional<FetchRules::Restart> restart =
        fetch_rules_->OnConnected(*request, AcceptCHFrame(info));
    if (fetch_rules_->request_restarts_exceeded()) {
      read_timer_.Stop();
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(&RequestJob::Finish, weak_ptr_factory_.GetWeakPtr(),
                         net::ERR_TOO_MANY_ACCEPT_CH_RESTARTS));
      return net::ERR_IO_PENDING;
    }
    if (restart) {
      read_timer_.Stop();
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(&RequestJob::RestartRequest,
                         weak_ptr_factory_.GetWeakPtr(), *std::move(restart)));
      return net::ERR_IO_PENDING;
    }
    return net::OK;
  }

  void OnReceivedRedirect(net::URLRequest* request,
                          const net::RedirectInfo& redirect_info,
                          bool* defer_redirect) override {
    // //net is still in the read that brought these headers. Over HTTP/2 it has
    // yet to see the END_STREAM of their frame, so going on from here would
    // cancel a stream that is about to end. Chrome's answer to a redirect comes
    // from a later task, so it never does.
    *defer_redirect = true;
    connect_timer_.Stop();
    read_timer_.Stop();
    // The next hop may reach the proxy another way, and be asked again.
    proxy_auth_answered_ = false;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&RequestJob::ContinueAfterRedirect,
                       weak_ptr_factory_.GetWeakPtr(), redirect_info));
  }

  void OnAuthRequired(net::URLRequest* request,
                      const net::AuthChallengeInfo& auth_info) override {
    // A server's challenge goes to the caller, as a response.
    if (!auth_info.is_proxy) {
      request->CancelAuth();
      return;
    }
    // Chrome would ask a person here. Asked again after an answer, the proxy
    // has refused it, and //net would ask for as long as this answers.
    if (!session_->proxy_credentials || proxy_auth_answered_) {
      request->CancelWithError(net::ERR_PROXY_AUTH_REQUESTED);
      return;
    }
    proxy_auth_answered_ = true;
    request->SetAuth(*session_->proxy_credentials);
  }

  void OnSSLCertificateError(net::URLRequest* request,
                             int net_error,
                             const net::SSLInfo& ssl_info,
                             bool fatal) override {
    // The default cancels with ERR_ABORTED, which would hide the reason.
    request->CancelWithError(net_error);
  }

  void OnResponseStarted(net::URLRequest* request, int net_error) override {
    if (net_error != net::OK) {
      Finish(net_error);
      return;
    }
    // A response from a cache comes with no connection.
    connect_timer_.Stop();
    read_timer_.Stop();
    if (fetch_rules_) {
      if (std::optional<FetchRules::Restart> restart =
              fetch_rules_->OnResponseHeaders(*request_);
          restart && !options_.stream_body) {
        // As with a redirect, Chrome answers this from a later task.
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE, base::BindOnce(&RequestJob::RestartRequest,
                                      weak_ptr_factory_.GetWeakPtr(),
                                      *std::move(restart)));
        return;
      }
    }
    PushResponseEvent(MITTO_EVENT_RESPONSE_STARTED);

    response_started_ = true;
    if (read_wanted_) {
      ReadBody();
    }
  }

  void OnReadCompleted(net::URLRequest* request, int bytes_read) override {
    read_timer_.Stop();
    if (bytes_read <= 0) {
      Finish(bytes_read);
      return;
    }
    channel_->BodyArrived(static_cast<size_t>(bytes_read));
    session_->ready_requests.Add(handle_);
  }

 private:
  // Returns a request that has what every request of this job has, and is not
  // set up any further.
  std::unique_ptr<net::URLRequest> CreateRequest(
      const GURL& url,
      const std::string& method,
      const net::HttpRequestHeaders& headers) {
    std::unique_ptr<net::URLRequest> request = session_->context->CreateRequest(
        url, options_.priority, this, kTrafficAnnotation,
        net::handles::kInvalidNetworkHandle);
    request->set_method(method);
    request->SetExtraRequestHeaders(headers);
    request->SetPriorityIncremental(options_.priority_incremental);
    return request;
  }

  void SetCallersReferrer() {
    if (!options_.referrer.is_empty()) {
      // The caller's, so no rule changes it, on any hop.
      request_->set_referrer_policy(net::ReferrerPolicy::NEVER_CLEAR);
      request_->SetReferrer(options_.referrer.spec());
    }
  }

  // What Chrome's network service makes of a body held in memory. //net adds
  // the Content-Length from it, and rewinds it for a redirect or a retry. The
  // job outlives the request, and a restart needs the body again.
  void SetBody() {
    request_->set_upload(net::ElementsUploadDataStream::CreateWithReader(
        std::make_unique<net::UploadBytesElementReader>(
            base::as_byte_span(*body_))));
  }

  std::string_view AcceptCHFrame(const net::TransportInfo& info) const {
    const auto found =
        session_->accept_ch_via_alps.find(url::Origin::Create(request_->url()));
    return found == session_->accept_ch_via_alps.end()
               ? std::string_view(info.accept_ch_frame)
               : std::string_view(found->second);
  }

  // Puts a new request in the place of the one that has to start over, as
  // ThrottlingURLLoader and NavigationURLLoaderImpl do. It only ever is a
  // navigation's, whose rules have said what it goes on with.
  void RestartRequest(FetchRules::Restart restart) {
    std::unique_ptr<net::URLRequest> old_request = std::move(request_);
    const bool has_body = old_request->has_upload();
    request_ =
        CreateRequest(restart.url, old_request->method(), restart.headers);
    fetch_rules_->ConfigureRestartedRequest(*request_, *old_request,
                                            restart.keeps_url_chain);
    old_request.reset();
    SetCallersReferrer();
    if (has_body) {
      SetBody();
    }
    proxy_auth_answered_ = false;
    StartConnectTimer();
    request_->Start();
  }

  // Pushes an event of `type` that describes the response so far.
  void PushResponseEvent(int32_t type) {
    RequestEvent event;
    event.type = type;
    event.status = request_->GetResponseCode();
    event.from_cache = request_->was_cached();
    event.protocol = net::HttpConnectionInfoToString(
        request_->response_info().connection_info);
    event.url = request_->url().spec();
    event.headers = HeaderLines(*request_->response_headers());
    channel_->Push(std::move(event));
    session_->ready_requests.Add(handle_);
  }

  // Goes on from a redirect that OnReceivedRedirect() deferred. This may delete
  // the job, so nothing may touch it afterwards.
  void ContinueAfterRedirect(const net::RedirectInfo& redirect_info) {
    if (fetch_rules_) {
      if (std::optional<FetchRules::Restart> restart =
              fetch_rules_->OnResponseHeaders(*request_);
          restart && !options_.stream_body) {
        RestartRequest(*std::move(restart));
        return;
      }
    }
    if (!options_.follow_redirects) {
      PushResponseEvent(MITTO_EVENT_RESPONSE_STARTED);
      // //net does not read the body of a redirect, so this is all there is.
      Finish(net::OK);
      return;
    }
    // Step 11 of https://fetch.spec.whatwg.org/#http-redirect-fetch, which Chrome
    // does in CorsURLLoader. A stream cannot start again, and a 303 is the one
    // redirect that needs no body. Once a 303 has dropped it, later hops have none
    // to send again.
    if (body_stream_ && request_->GetResponseCode() != net::HTTP_SEE_OTHER) {
      Finish(net::ERR_UPLOAD_STREAM_REWIND_NOT_SUPPORTED);
      return;
    }
    PushResponseEvent(MITTO_EVENT_REDIRECT_FOLLOWED);

    // Step 13 of https://fetch.spec.whatwg.org/#http-redirect-fetch, which
    // Chrome does in ResourceLoader::WillFollowRedirect() in Blink. The header
    // stays gone if a later hop comes back to this origin. Fetch does not name
    // Cookie, because a page cannot send one, but a caller's has no domain to
    // keep it where it belongs. //net adds the store's cookies on each hop, so
    // this does not touch them.
    std::vector<std::string> removed_headers;
    if (!url::IsSameOriginWith(request_->url(), redirect_info.new_url)) {
      removed_headers.emplace_back(net::HttpRequestHeaders::kAuthorization);
      removed_headers.emplace_back(net::HttpRequestHeaders::kCookie);
    }
    net::HttpRequestHeaders modified_headers;
    if (fetch_rules_) {
      fetch_rules_->WillFollowRedirect(*request_, redirect_info,
                                       removed_headers, modified_headers);
    }
    // The next hop has its own connection to get.
    StartConnectTimer();
    request_->FollowDeferredRedirect(removed_headers, modified_headers);
  }

  void StartConnectTimer() {
    if (options_.connect_timeout) {
      connect_timer_.Start(FROM_HERE, *options_.connect_timeout,
                           base::BindOnce(&RequestJob::Finish,
                                          base::Unretained(this),
                                          net::ERR_CONNECTION_TIMED_OUT));
    }
  }

  void StartReadTimer() {
    if (options_.read_timeout) {
      received_bytes_at_read_timer_start_ = request_->GetTotalReceivedBytes();
      sent_bytes_at_read_timer_start_ = request_->GetTotalSentBytes();
      read_timer_.Start(FROM_HERE, *options_.read_timeout,
                        base::BindOnce(&RequestJob::OnReadTimerFired,
                                       base::Unretained(this)));
    }
  }

  void OnReadTimerFired() {
    // A content decoder can take bytes and give nothing back yet, and then //net
    // does not complete the read, although the server is still sending. A server
    // that is still taking the body has no reason to answer yet either.
    if (request_->GetTotalReceivedBytes() !=
            received_bytes_at_read_timer_start_ ||
        request_->GetTotalSentBytes() != sent_bytes_at_read_timer_start_) {
      StartReadTimer();
      return;
    }
    Finish(net::ERR_TIMED_OUT);
  }

  // `result` is net::OK when the body ended, otherwise a net error. This deletes
  // the job, so nothing may touch it afterwards.
  void Finish(int result) {
    // Deleting the request from its delegate's callback is allowed.
    request_.reset();
    channel_->Finish(result);
    session_->ready_requests.Add(handle_);
    session_->jobs.erase(session_->jobs.find(this));
  }

  const raw_ref<MittoSession> session_;
  const uintptr_t handle_;
  const scoped_refptr<RequestChannel> channel_;
  const GURL url_;
  const net::HttpRequestHeaders request_headers_;
  RequestOptions options_;
  // What a request made for a page does at its start and on each hop.
  std::optional<FetchRules> fetch_rules_;
  // What to send as the body, which `request_` reads from.
  std::optional<std::vector<char>> body_;
  // The stream body, which `request_` owns and drops on a 303.
  base::WeakPtr<BodyStream> body_stream_;
  std::unique_ptr<net::URLRequest> request_;
  bool response_started_ = false;
  bool read_wanted_ = false;
  bool proxy_auth_answered_ = false;
  base::OneShotTimer connect_timer_;
  base::OneShotTimer read_timer_;
  base::OneShotTimer total_timer_;
  base::ByteSize received_bytes_at_read_timer_start_;
  base::ByteSize sent_bytes_at_read_timer_start_;
  base::WeakPtrFactory<RequestJob> weak_ptr_factory_{this};
};

}  // namespace mitto

MittoSession::MittoSession() = default;

MittoSession::~MittoSession() {
  // This ends its requests, which have to go before `context` does. A worker
  // thread that is waiting for one of them goes on with an error, and may hold
  // the fetcher for longer, but it starts nothing more.
  cert_net_fetcher->Shutdown();
}

void MittoSession::CancelJobs() {
  while (!jobs.empty()) {
    (*jobs.begin())->Cancel();
  }
}

namespace mitto {
namespace {

// As NetworkContext::ActivateDohProbes() does it. Without probes, secure mode
// automatic never finds a DoH server that it can use.
void ActivateDohProbes(MittoSession* session) {
  session->doh_probes_request =
      session->context->host_resolver()->CreateDohProbeRequest();
  session->doh_probes_request->Start();
}

// What CertVerifier::CreateDefault() makes and what NetworkContext wraps
// Chrome's verifier in, with the trust anchors that a Session adds. //net's own
// factory still chooses the rest, and rebuilds the verifier's CertVerifyProc for
// them.
std::unique_ptr<net::CertVerifier> CreateCertVerifier(
    scoped_refptr<net::CertNetFetcher> cert_net_fetcher,
    bssl::ParsedCertificateList additional_trust_anchors) {
  std::unique_ptr<net::CertVerifierWithUpdatableProc> verifier =
      net::CertVerifier::CreateDefaultWithoutCaching(cert_net_fetcher);
  if (!additional_trust_anchors.empty()) {
    net::CertVerifyProc::InstanceParams instance_params;
    instance_params.additional_trust_anchors =
        std::move(additional_trust_anchors);
    verifier->UpdateVerifyProcData(std::move(cert_net_fetcher),
                                   net::CertVerifyProc::ImplParams(),
                                   instance_params);
  }
  return std::make_unique<net::CachingCertVerifier>(
      std::make_unique<net::CoalescingCertVerifier>(std::move(verifier)));
}

// Makes `directory` if it does not exist and takes a lock on it. Chrome relies
// on being one process, and //net on one backend for each directory. A second
// one in the same process would wait for the first to go, so its requests would
// never end, and one in another process would overwrite the first's files.
int LockHttpCacheDirectory(const base::FilePath& directory,
                           DirectoryLock& lock) {
  if (!base::CreateDirectory(directory)) {
    return net::ERR_CACHE_OPEN_FAILURE;
  }
  switch (lock.Take(directory)) {
    case DirectoryLock::Result::kTaken:
      return net::OK;
    case DirectoryLock::Result::kHeld:
      return net::ERR_CACHE_LOCK_TIMEOUT;
    case DirectoryLock::Result::kFailed:
      return net::ERR_CACHE_OPEN_FAILURE;
  }
  NOTREACHED();
}

// Blockfile is what ChooseCacheType() in //components picks on Windows, the
// system of every Persona, while macOS and Linux get the simple backend there.
// It shows on the wire. Its writes never finish at once, so over HTTP/2 a 304
// has ended its stream by the time HttpCache lets the network transaction go.
// The simple backend and the one in memory can finish at once, so they cancel
// that stream, with an RST_STREAM that no Baseline holds. Its destructor also
// waits for its work, so a process can end right after destroying a Session.
//
// A directory of //net's own keeps the caller's files apart from it, because a
// cache that //net cannot read makes it rename the whole directory and delete
// it. Chrome also gives //net a directory for No-Vary-Search, whose writes go on
// after a Session has gone, so here those entries last as long as the Session.
void ConfigureHttpCache(net::URLRequestContextBuilder& builder,
                        const SessionSettings& settings) {
  if (!settings.http_cache_in_memory && !settings.http_cache_directory) {
    builder.DisableHttpCache();
    return;
  }
  net::URLRequestContextBuilder::HttpCacheParams params;
  if (settings.http_cache_directory) {
    params.type = net::URLRequestContextBuilder::HttpCacheParams::DISK_BLOCKFILE;
    params.path = settings.http_cache_directory->Append(kCacheDataDirectoryName);
  }
  builder.EnableHttpCache(params);
}

// Called on the network thread.
std::unique_ptr<MittoSession> CreateSessionOnNetworkThread(
    SessionSettings settings,
    DirectoryLock http_cache_directory_lock,
    net::HostResolverManager* host_resolver_manager,
    base::TimeTicks doh_probes_start_time) {
  auto session = std::make_unique<MittoSession>();
  session->http_cache_directory_lock = std::move(http_cache_directory_lock);
  session->cert_net_fetcher =
      base::MakeRefCounted<net::CertNetFetcherURLRequest>();
  if (settings.dns_nameserver) {
    session->own_host_resolver_manager =
        std::make_unique<net::HostResolverManager>(
            net::HostResolver::ManagerOptions(),
            net::NetworkChangeNotifier::GetSystemDnsConfigNotifier(),
            net::NetLog::Get());
    host_resolver_manager = session->own_host_resolver_manager.get();
    ConfigureStubHostResolver(*host_resolver_manager, settings.dns_nameserver);
  }

  net::URLRequestContextBuilder builder;
  builder.set_host_resolver_manager(host_resolver_manager);
  ConfigureHttpCache(builder, settings);
  // Chrome's network service turns these on. They add to what //net puts in
  // Accept-Encoding, over HTTPS and to localhost only.
  builder.set_enable_brotli(true);
  builder.set_enable_zstd(true);
  builder.set_accept_language(settings.accept_language);
  builder.set_user_agent(settings.user_agent);
  builder.set_host_mapping_rules(std::move(settings.host_resolver_rules));
  // Only what the settings name, so that the system's settings stay ignored.
  builder.set_proxy_config_service(
      std::make_unique<net::ProxyConfigServiceFixed>(
          net::ProxyConfigWithAnnotation(settings.proxy_config,
                                         kProxyTrafficAnnotation)));
  session->proxy_credentials = std::move(settings.proxy_credentials);
  session->accept_ch_via_alps = std::move(settings.accept_ch_via_alps);
  session->network_quality = std::move(settings.network_quality);
  // Only for a Session that needs one, so that no other has anything between
  // //net and its sockets: one with a SOCKS5 proxy and credentials, and on
  // Windows every Session, whose connects the system refuses now and then.
  std::unique_ptr<net::ClientSocketFactory> socket_factory;
  if (settings.socks5_credentials) {
    socket_factory = std::make_unique<Socks5AuthSocketFactory>(
        *std::move(settings.socks5_credentials));
  }
  if (BUILDFLAG(IS_WIN) || settings.refused_connects > 0) {
    socket_factory = std::make_unique<ConnectRetryingSocketFactory>(
        std::move(socket_factory), settings.refused_connects);
  }
  if (socket_factory) {
    builder.set_client_socket_factory(std::move(socket_factory));
  }
  std::unique_ptr<net::CertVerifier> cert_verifier = CreateCertVerifier(
      session->cert_net_fetcher, std::move(settings.additional_trust_anchors));
  if (!settings.ignore_certificate_errors_spki_list.empty()) {
    cert_verifier = std::make_unique<IgnoreErrorsCertVerifier>(
        std::move(cert_verifier),
        std::move(settings.ignore_certificate_errors_spki_list));
  }
  builder.SetCertVerifier(std::move(cert_verifier));
  builder.set_ssl_config_service(
      std::make_unique<SSLConfigServiceMojo>(GetNewSSLConfig()));

  net::HttpNetworkSessionParams session_params;
  auto quic_context = std::make_unique<net::QuicContext>();
  ConfigureQuicParams(&session_params, quic_context->params());
  ConfigureHttp2Params(&session_params);
  quic_context->params()->origins_to_force_quic_on =
      std::move(settings.origins_to_force_quic_on);
  quic_context->params()->force_quic_everywhere =
      settings.force_quic_everywhere;
  builder.set_http_network_session_params(session_params);
  builder.set_quic_context(std::move(quic_context));

  session->context = builder.Build();
  session->cert_net_fetcher->SetURLRequestContext(session->context.get());
  ConfigureQuicHints(session->context->http_server_properties());
  // The timer belongs to the Session, so it can only run while the Session
  // exists. NetworkService does this with one timer and a list of contexts.
  session->doh_probes_timer.Start(
      FROM_HERE,
      std::max(doh_probes_start_time - base::TimeTicks::Now(),
               base::TimeDelta()),
      base::BindOnce(&ActivateDohProbes, base::Unretained(session.get())));
  return session;
}

// Process-wide state: what //base needs before //net can run, and the network
// thread that all Sessions live on. It is never destroyed, because the library
// cannot know when the process that loaded it stops using it. That is why the
// library is built to stay loaded until the process exits.
class NetworkCore final : public net::SystemDnsConfigChangeNotifier::Observer {
 public:
  static NetworkCore& Get() {
    static base::NoDestructor<NetworkCore> instance;
    return *instance;
  }

  NetworkCore() : network_thread_("mitto network") {
#if BUILDFLAG(IS_WIN)
    // Before any thread starts.
    PinLibrary();
#endif
    // Failures reach the caller as return values. Logging them as well would
    // write to the stderr of a program that did not ask for it.
    logging::SetMinLogLevel(logging::LOGGING_FATAL);
    // //net reads the command line, but the arguments of the process that
    // loaded the library are not meant for it.
    base::CommandLine::Init(0, nullptr);
    InitFeatureList();
    // What ChromeMainDelegate::CommonEarlyInitialization() does, in its order.
    // These need the feature list, and have to come before anything asks whether
    // the cache is split or network state partitioned, and before any cache
    // exists.
    net::HttpCache::SplitCacheFeatureEnableByDefault();
    net::NetworkAnonymizationKey::PartitionByDefault();
    base::ThreadPoolInstance::CreateAndStartWithDefaultParams("mitto");
    CHECK(network_thread_.StartWithOptions(
        base::Thread::Options(base::MessagePumpType::IO, 0)));
    RunAndWait(base::BindOnce(&NetworkCore::CreateNetworkChangeNotifier,
                              base::Unretained(this)));
    // Chrome has read these settings by the time a page asks for a host, but a
    // program's first request can come right away. Until the
    // HostResolverManager has them it goes to the system's resolver, which sees
    // no HTTPS records.
    system_dns_config_read_.TimedWait(kSystemDnsConfigTimeout);
    RunAndWait(base::BindOnce(&NetworkCore::CreateHostResolverManager,
                              base::Unretained(this)));
  }

  // net::SystemDnsConfigChangeNotifier::Observer:
  void OnSystemDnsConfigChanged(const net::DnsConfig& config) override {
    system_dns_config_read_.Signal();
  }

  std::unique_ptr<MittoSession> CreateSession(
      SessionSettings settings,
      DirectoryLock http_cache_directory_lock) {
    std::unique_ptr<MittoSession> session;
    RunAndWait(base::BindOnce(
        [](NetworkCore* core, SessionSettings settings,
           DirectoryLock http_cache_directory_lock,
           std::unique_ptr<MittoSession>* session) {
          *session = CreateSessionOnNetworkThread(
              std::move(settings), std::move(http_cache_directory_lock),
              core->host_resolver_manager_.get(), core->doh_probes_start_time_);
        },
        base::Unretained(this), std::move(settings),
        std::move(http_cache_directory_lock), base::Unretained(&session)));
    return session;
  }

  // Waits until `session`'s HTTP cache, if any, can take its first request, and
  // returns a net error if it never can. //net opens a cache for the first
  // request that wants it, and goes on without one if that fails, which nothing
  // would tell a caller.
  int OpenHttpCache(MittoSession& session) {
    int result = net::OK;
    base::WaitableEvent done;
    PostTask(base::BindOnce(
        [](MittoSession* session, int* result, base::WaitableEvent* done) {
          net::HttpCache* const cache =
              session->context->http_transaction_factory()->GetCache();
          if (!cache) {
            done->Signal();
            return;
          }
          auto [from_callback, from_here] = base::SplitOnceCallback(
              base::BindOnce(
                  [](int* result, base::WaitableEvent* done,
                     net::HttpCache::GetBackendResult backend) {
                    if (backend.first != net::OK) {
                      *result = net::ERR_CACHE_OPEN_FAILURE;
                    }
                    done->Signal();
                  },
                  base::Unretained(result), base::Unretained(done)));
          net::HttpCache::GetBackendResult backend =
              cache->GetBackend(std::move(from_callback));
          if (backend.first != net::ERR_IO_PENDING) {
            std::move(from_here).Run(std::move(backend));
          }
        },
        base::Unretained(&session), base::Unretained(&result),
        base::Unretained(&done)));
    done.Wait();
    return result;
  }

  void DestroySession(std::unique_ptr<MittoSession> session) {
    RunAndWait(base::BindOnce(
        [](std::unique_ptr<MittoSession> session) { session->CancelJobs(); },
        std::move(session)));
  }

  void StartJob(MittoSession& session, std::unique_ptr<RequestJob> job) {
    PostTask(base::BindOnce(
        [](MittoSession* session, std::unique_ptr<RequestJob> job) {
          RequestJob* const started_job = job.get();
          session->jobs.insert(std::move(job));
          started_job->Start();
        },
        base::Unretained(&session), std::move(job)));
  }

  std::string ListCookies(MittoSession& session) {
    std::string cookies;
    base::WaitableEvent done;
    PostTask(base::BindOnce(
        [](MittoSession* session, std::string* cookies,
           base::WaitableEvent* done) {
          session->context->cookie_store()->GetAllCookiesAsync(base::BindOnce(
              [](std::string* cookies, base::WaitableEvent* done,
                 const net::CookieList& list) {
                *cookies = WriteCookies(list);
                done->Signal();
              },
              base::Unretained(cookies), base::Unretained(done)));
        },
        base::Unretained(&session), base::Unretained(&cookies),
        base::Unretained(&done)));
    done.Wait();
    return cookies;
  }

  // Returns whether the store took every one of them. ParseCookies() has
  // already asked it, so this only guards against //net changing its mind.
  bool SetCookies(MittoSession& session, std::vector<CookieToSet> cookies) {
    bool all_set = true;
    base::WaitableEvent done;
    PostTask(base::BindOnce(
        [](MittoSession* session, std::vector<CookieToSet> cookies,
           bool* all_set, base::WaitableEvent* done) {
          const base::RepeatingClosure one_done = base::BarrierClosure(
              cookies.size(), base::BindOnce(&base::WaitableEvent::Signal,
                                             base::Unretained(done)));
          for (CookieToSet& cookie : cookies) {
            // The caller owns this store, so no rule about where a request
            // comes from stands in its way.
            session->context->cookie_store()->SetCanonicalCookieAsync(
                std::move(cookie.cookie), cookie.source_url,
                net::CookieOptions::MakeAllInclusive(),
                base::BindOnce(
                    [](bool* all_set, const base::RepeatingClosure& one_done,
                       net::CookieAccessResult result) {
                      *all_set = *all_set && result.status.IsInclude();
                      one_done.Run();
                    },
                    base::Unretained(all_set), one_done),
                /*cookie_access_result=*/std::nullopt);
          }
        },
        base::Unretained(&session), std::move(cookies),
        base::Unretained(&all_set), base::Unretained(&done)));
    done.Wait();
    return all_set;
  }

  // Returns how many cookies went.
  uint32_t DeleteCookies(MittoSession& session, CookieFilter filter) {
    uint32_t deleted = 0;
    base::WaitableEvent done;
    PostTask(base::BindOnce(
        [](MittoSession* session, CookieFilter filter, uint32_t* deleted,
           base::WaitableEvent* done) {
          session->context->cookie_store()->DeleteMatchingCookiesAsync(
              base::BindRepeating(
                  &CookieFilter::Matches,
                  base::Owned(
                      std::make_unique<CookieFilter>(std::move(filter)))),
              base::BindOnce(
                  [](uint32_t* deleted, base::WaitableEvent* done,
                     uint32_t count) {
                    *deleted = count;
                    done->Signal();
                  },
                  base::Unretained(deleted), base::Unretained(done)));
        },
        base::Unretained(&session), std::move(filter),
        base::Unretained(&deleted), base::Unretained(&done)));
    done.Wait();
    return deleted;
  }

  // Runs `task` on the network thread and returns without waiting for it.
  void PostTask(base::OnceClosure task) {
    network_thread_.task_runner()->PostTask(FROM_HERE, std::move(task));
  }

 private:
  // As the network service does it, on the thread that uses it. This also
  // starts the reading of the system's DNS settings.
  void CreateNetworkChangeNotifier() {
    network_change_notifier_ = net::NetworkChangeNotifier::CreateIfNeeded();
    net::NetworkChangeNotifier::GetSystemDnsConfigNotifier()->AddObserver(this);
  }

  // If the settings have arrived, the new HostResolverManager's copy of them is
  // now in the network thread's queue, ahead of any task that creates a
  // Session.
  void CreateHostResolverManager() {
    net::SystemDnsConfigChangeNotifier* const notifier =
        net::NetworkChangeNotifier::GetSystemDnsConfigNotifier();
    notifier->RemoveObserver(this);
    host_resolver_manager_ = std::make_unique<net::HostResolverManager>(
        net::HostResolver::ManagerOptions(), notifier, net::NetLog::Get());
    ConfigureStubHostResolver(*host_resolver_manager_,
                              /*dns_nameserver=*/std::nullopt);
    doh_probes_start_time_ =
        base::TimeTicks::Now() + kDelayInitialDohProbeTimeout;
  }

  void RunAndWait(base::OnceClosure task) {
    base::WaitableEvent done;
    network_thread_.task_runner()->PostTask(
        FROM_HERE, std::move(task).Then(base::BindOnce(
                       &base::WaitableEvent::Signal, base::Unretained(&done))));
    done.Wait();
  }

  base::AtExitManager at_exit_manager_;
  base::Thread network_thread_;
  base::WaitableEvent system_dns_config_read_;

  // These are used only on the network thread.
  std::unique_ptr<net::NetworkChangeNotifier> network_change_notifier_;
  std::unique_ptr<net::HostResolverManager> host_resolver_manager_;
  base::TimeTicks doh_probes_start_time_;
};

// Returns `headers` in the same order, or nullopt if they cannot be sent as
// given.
std::optional<net::HttpRequestHeaders> ToRequestHeaders(
    base::span<const MittoHeader> headers) {
  net::HttpRequestHeaders request_headers;
  for (const MittoHeader& header : headers) {
    if (!header.name || !header.value) {
      return std::nullopt;
    }
    // SAFETY: The C API says each string holds the size that goes with it.
    const std::string_view name =
        UNSAFE_BUFFERS(std::string_view(header.name, header.name_size));
    const std::string_view value =
        UNSAFE_BUFFERS(std::string_view(header.value, header.value_size));
    // SetHeader() CHECKs that both are valid, and a name it already holds would
    // silently take the new value at the old position. //net would silently
    // drop a Referer, because it only sends the one from SetReferrer().
    if (!net::HttpUtil::IsValidHeaderName(name) ||
        !net::HttpUtil::IsValidHeaderValue(value) ||
        request_headers.HasHeader(name) ||
        base::EqualsCaseInsensitiveASCII(name,
                                         net::HttpRequestHeaders::kReferer)) {
      return std::nullopt;
    }
    request_headers.SetHeader(name, value);
  }
  return request_headers;
}

// Copies `data` into a malloc'd buffer with a trailing NUL byte.
char* CopyToNewBuffer(std::string_view data) {
  const size_t buffer_size = data.size() + 1;
  char* const buffer = static_cast<char*>(malloc(buffer_size));
  CHECK(buffer);
  // SAFETY: `buffer` was just allocated with `buffer_size` bytes.
  base::span<char> output = UNSAFE_BUFFERS(base::span(buffer, buffer_size));
  output.copy_prefix_from(data);
  output[data.size()] = '\0';
  return buffer;
}

}  // namespace
}  // namespace mitto

extern "C" {

uint32_t mitto_abi_version(void) {
  return MITTO_ABI_VERSION;
}

const char* mitto_net_error_name(int32_t net_error) {
  // The list and not net::ErrorToShortString(), which reports an unknown value
  // as a bug in //net. Here it comes from outside the library.
  switch (net_error) {
    case net::OK:
      return "OK";
#define NET_ERROR(label, value) \
  case net::ERR_##label:        \
    return "ERR_" #label;
#include "net/base/net_error_list.h"
#undef NET_ERROR
    default:
      return "ERR_<unknown>";
  }
}

int32_t mitto_session_create(const char* settings,
                             size_t settings_size,
                             MittoSession** session) {
  if (!session) {
    return net::ERR_INVALID_ARGUMENT;
  }
  *session = nullptr;
  if (!settings) {
    return net::ERR_INVALID_ARGUMENT;
  }

  mitto::NetworkCore& core = mitto::NetworkCore::Get();
  // SAFETY: The C API says `settings` holds `settings_size` bytes.
  std::optional<mitto::SessionSettings> parsed = mitto::ParseSessionSettings(
      UNSAFE_BUFFERS(std::string_view(settings, settings_size)));
  if (!parsed) {
    return net::ERR_INVALID_ARGUMENT;
  }
  mitto::DirectoryLock http_cache_directory_lock;
  if (parsed->http_cache_directory) {
    const int result = mitto::LockHttpCacheDirectory(
        *parsed->http_cache_directory, http_cache_directory_lock);
    if (result != net::OK) {
      return result;
    }
  }
  std::unique_ptr<MittoSession> created = core.CreateSession(
      std::move(*parsed), std::move(http_cache_directory_lock));
  const int result = core.OpenHttpCache(*created);
  if (result != net::OK) {
    core.DestroySession(std::move(created));
    return result;
  }
  *session = created.release();
  return net::OK;
}

void mitto_session_destroy(MittoSession* session) {
  if (session) {
    session->ready_requests.EndWaitsAndWaitForThem();
    mitto::NetworkCore::Get().DestroySession(base::WrapUnique(session));
  }
}

int32_t mitto_session_list_cookies(MittoSession* session,
                                   char** cookies,
                                   size_t* cookies_size) {
  if (!cookies || !cookies_size) {
    return net::ERR_INVALID_ARGUMENT;
  }
  *cookies = nullptr;
  *cookies_size = 0;
  if (!session) {
    return net::ERR_INVALID_ARGUMENT;
  }
  const std::string json = mitto::NetworkCore::Get().ListCookies(*session);
  *cookies = mitto::CopyToNewBuffer(json);
  *cookies_size = json.size();
  return net::OK;
}

int32_t mitto_session_set_cookies(MittoSession* session,
                                  const char* cookies,
                                  size_t cookies_size) {
  if (!session || !cookies) {
    return net::ERR_INVALID_ARGUMENT;
  }
  mitto::NetworkCore& core = mitto::NetworkCore::Get();
  // SAFETY: The C API says `cookies` holds `cookies_size` bytes.
  std::optional<std::vector<mitto::CookieToSet>> parsed = mitto::ParseCookies(
      UNSAFE_BUFFERS(std::string_view(cookies, cookies_size)));
  if (!parsed || !core.SetCookies(*session, *std::move(parsed))) {
    return net::ERR_INVALID_ARGUMENT;
  }
  return net::OK;
}

int32_t mitto_session_delete_cookies(MittoSession* session,
                                     const char* filter,
                                     size_t filter_size,
                                     uint32_t* deleted) {
  if (!deleted) {
    return net::ERR_INVALID_ARGUMENT;
  }
  *deleted = 0;
  if (!session || !filter) {
    return net::ERR_INVALID_ARGUMENT;
  }
  mitto::NetworkCore& core = mitto::NetworkCore::Get();
  // SAFETY: The C API says `filter` holds `filter_size` bytes.
  std::optional<mitto::CookieFilter> parsed = mitto::ParseCookieFilter(
      UNSAFE_BUFFERS(std::string_view(filter, filter_size)));
  if (!parsed) {
    return net::ERR_INVALID_ARGUMENT;
  }
  *deleted = core.DeleteCookies(*session, *std::move(parsed));
  return net::OK;
}

void mitto_cookies_free(char* cookies) {
  free(cookies);
}

int32_t mitto_session_event_fd(MittoSession* session, int32_t* fd) {
  if (!fd) {
    return net::ERR_INVALID_ARGUMENT;
  }
  *fd = -1;
  if (!session) {
    return net::ERR_INVALID_ARGUMENT;
  }
#if BUILDFLAG(IS_WIN)
  return net::ERR_NOT_IMPLEMENTED;
#else
  const std::optional<int> descriptor =
      session->ready_requests.GetFileDescriptor();
  if (!descriptor) {
    return net::ERR_INSUFFICIENT_RESOURCES;
  }
  *fd = *descriptor;
  return net::OK;
#endif
}

int32_t mitto_session_next_ready_request(MittoSession* session,
                                         MittoRequest** request) {
  if (!request) {
    return net::ERR_INVALID_ARGUMENT;
  }
  *request = nullptr;
  if (!session) {
    return net::ERR_INVALID_ARGUMENT;
  }
  const std::optional<uintptr_t> ready = session->ready_requests.Take();
  if (!ready) {
    return net::ERR_IO_PENDING;
  }
  *request = reinterpret_cast<MittoRequest*>(*ready);
  return net::OK;
}

int32_t mitto_session_wait_for_event(MittoSession* session,
                                     int64_t timeout_ms) {
  if (!session) {
    return net::ERR_INVALID_ARGUMENT;
  }
  std::optional<base::TimeDelta> timeout;
  if (timeout_ms >= 0) {
    timeout = base::Milliseconds(timeout_ms);
  }
  return session->ready_requests.Wait(timeout);
}

void mitto_session_end_waits(MittoSession* session) {
  if (session) {
    session->ready_requests.EndWaits();
  }
}

int32_t mitto_request_start(MittoSession* session,
                            const char* url,
                            const MittoHeader* headers,
                            size_t header_count,
                            const char* options,
                            size_t options_size,
                            const uint8_t* body,
                            size_t body_size,
                            MittoRequest** request) {
  if (!request) {
    return net::ERR_INVALID_ARGUMENT;
  }
  *request = nullptr;
  if (!session || !url || (!headers && header_count != 0) || !options ||
      (!body && body_size != 0)) {
    return net::ERR_INVALID_ARGUMENT;
  }
  // SAFETY: The C API says `headers` holds `header_count` elements. A span can
  // be null only if it is empty, which the check above makes sure of.
  std::optional<net::HttpRequestHeaders> request_headers =
      mitto::ToRequestHeaders(
          UNSAFE_BUFFERS(base::span(headers, header_count)));
  // SAFETY: The C API says `options` holds `options_size` bytes.
  std::optional<mitto::RequestOptions> parsed_options =
      mitto::ParseRequestOptions(
          UNSAFE_BUFFERS(std::string_view(options, options_size)));
  if (!request_headers || !parsed_options ||
      (parsed_options->stream_body && body)) {
    return net::ERR_INVALID_ARGUMENT;
  }

  std::optional<std::vector<char>> request_body;
  if (body) {
    // SAFETY: The C API says `body` holds `body_size` bytes.
    const base::span<const char> bytes =
        base::as_chars(UNSAFE_BUFFERS(base::span(body, body_size)));
    request_body.emplace(bytes.begin(), bytes.end());
  }

  auto handle = std::make_unique<MittoRequest>();
  handle->channel = base::MakeRefCounted<mitto::RequestChannel>();
  if (parsed_options->stream_body) {
    handle->channel->OpenBody(parsed_options->stream_body_size);
  }
  auto job = std::make_unique<mitto::RequestJob>(
      *session, reinterpret_cast<uintptr_t>(handle.get()), handle->channel,
      GURL(url), std::move(*request_headers), std::move(*parsed_options),
      std::move(request_body));
  handle->job = job->GetWeakPtr();
  mitto::NetworkCore::Get().StartJob(*session, std::move(job));
  *request = handle.release();
  return net::OK;
}

int32_t mitto_request_next_event(MittoRequest* request,
                                 int64_t timeout_ms,
                                 MittoEvent* event) {
  if (!event) {
    return net::ERR_INVALID_ARGUMENT;
  }
  *event = {};
  if (!request) {
    return net::ERR_INVALID_ARGUMENT;
  }
  std::optional<base::TimeDelta> timeout;
  if (timeout_ms >= 0) {
    timeout = base::Milliseconds(timeout_ms);
  }
  mitto::RequestEvent next;
  const int result = request->channel->NextEvent(timeout, next);
  if (result != net::OK) {
    return result;
  }
  event->type = next.type;
  event->status = next.status;
  event->net_error = next.net_error;
  if (next.type == MITTO_EVENT_RESPONSE_STARTED) {
    event->from_cache = next.from_cache;
    event->protocol = mitto::CopyToNewBuffer(next.protocol);
  }
  if (next.type == MITTO_EVENT_RESPONSE_STARTED ||
      next.type == MITTO_EVENT_REDIRECT_FOLLOWED) {
    event->url = mitto::CopyToNewBuffer(next.url);
    event->headers = mitto::CopyToNewBuffer(next.headers);
    event->headers_size = next.headers.size();
  }
  return net::OK;
}

int32_t mitto_request_read(MittoRequest* request,
                           uint8_t* buffer,
                           size_t size) {
  if (!request || !buffer || size == 0) {
    return net::ERR_INVALID_ARGUMENT;
  }
  bool start_reading = false;
  // SAFETY: The C API says `buffer` holds `size` bytes.
  const int result = request->channel->Read(
      UNSAFE_BUFFERS(base::span(buffer, size)), start_reading);
  if (start_reading) {
    mitto::NetworkCore::Get().PostTask(
        base::BindOnce(&mitto::RequestJob::ReadBody, request->job));
  }
  return result;
}

int32_t mitto_request_write_body(MittoRequest* request,
                                 const uint8_t* data,
                                 size_t size) {
  if (!request || !data || size == 0) {
    return net::ERR_INVALID_ARGUMENT;
  }
  bool wake_network_thread = false;
  // SAFETY: The C API says `data` holds `size` bytes.
  const int result = request->channel->WriteBody(
      UNSAFE_BUFFERS(base::span(data, size)), wake_network_thread);
  if (wake_network_thread) {
    mitto::NetworkCore::Get().PostTask(
        base::BindOnce(&mitto::RequestJob::SendBody, request->job));
  }
  return result;
}

int32_t mitto_request_end_body(MittoRequest* request) {
  if (!request) {
    return net::ERR_INVALID_ARGUMENT;
  }
  bool wake_network_thread = false;
  const int result = request->channel->EndBody(wake_network_thread);
  if (wake_network_thread) {
    mitto::NetworkCore::Get().PostTask(
        base::BindOnce(&mitto::RequestJob::SendBody, request->job));
  }
  return result;
}

void mitto_request_cancel(MittoRequest* request) {
  if (request) {
    mitto::NetworkCore::Get().PostTask(
        base::BindOnce(&mitto::RequestJob::Cancel, request->job));
  }
}

void mitto_request_release(MittoRequest* request) {
  if (request) {
    mitto_request_cancel(request);
    delete request;
  }
}

void mitto_event_free(MittoEvent* event) {
  if (!event) {
    return;
  }
  free(event->protocol);
  free(event->url);
  free(event->headers);
  *event = {};
}

}  // extern "C"
