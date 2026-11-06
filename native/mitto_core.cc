#include <stdint.h>
#include <stdlib.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "base/at_exit.h"
#include "base/byte_size.h"
#include "base/check.h"
#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/containers/unique_ptr_adapters.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/memory/raw_ref.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/message_loop/message_pump_type.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "mitto/feature_list.h"
#include "mitto/ignore_errors_cert_verifier.h"
#include "mitto/mitto.h"
#include "mitto/network_session_configurator.h"
#include "mitto/ready_requests.h"
#include "mitto/request_channel.h"
#include "mitto/request_options.h"
#include "mitto/session_settings.h"
#include "mitto/ssl_config_service_mojo.h"
#include "mitto/stub_resolver_config_reader.h"
#include "net/base/completion_once_callback.h"
#include "net/base/io_buffer.h"
#include "net/base/net_errors.h"
#include "net/base/network_change_notifier.h"
#include "net/base/network_handle.h"
#include "net/base/request_priority.h"
#include "net/base/transport_info.h"
#include "net/cert/caching_cert_verifier.h"
#include "net/cert/cert_net_fetcher.h"
#include "net/cert/cert_verifier.h"
#include "net/cert/cert_verify_proc.h"
#include "net/cert/coalescing_cert_verifier.h"
#include "net/cert_net/cert_net_fetcher_url_request.h"
#include "net/dns/host_resolver.h"
#include "net/dns/host_resolver_manager.h"
#include "net/dns/system_dns_config_change_notifier.h"
#include "net/http/http_connection_info.h"
#include "net/http/http_network_session.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_response_info.h"
#include "net/http/http_util.h"
#include "net/log/net_log.h"
#include "net/proxy_resolution/proxy_config_service_fixed.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"
#include "net/quic/quic_context.h"
#include "net/ssl/ssl_info.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/redirect_info.h"
#include "net/url_request/url_request.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_builder.h"
#include "url/gurl.h"

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

  // The resolver of a Session with a DNS server of its own, which the others
  // must not ask. `context` points to it, so it has to come first.
  std::unique_ptr<net::HostResolverManager> own_host_resolver_manager;
  std::unique_ptr<net::URLRequestContext> context;
  // The verifier in `context` holds this, and this makes its requests in
  // `context`, until the destructor shuts it down.
  scoped_refptr<net::CertNetFetcherURLRequest> cert_net_fetcher;
  std::unique_ptr<net::HostResolver::ProbeRequest> doh_probes_request;
  base::OneShotTimer doh_probes_timer;
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

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation(
        "mitto_request",
        "A request made by the application that loaded the library.");

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
class RequestJob final : public net::URLRequest::Delegate {
 public:
  // `handle` names the request in the Session's ReadyRequests.
  RequestJob(MittoSession& session,
             uintptr_t handle,
             scoped_refptr<RequestChannel> channel,
             GURL url,
             net::HttpRequestHeaders headers,
             RequestOptions options)
      : session_(session),
        handle_(handle),
        channel_(std::move(channel)),
        url_(std::move(url)),
        request_headers_(std::move(headers)),
        options_(std::move(options)) {}

  RequestJob(const RequestJob&) = delete;
  RequestJob& operator=(const RequestJob&) = delete;

  ~RequestJob() override = default;

  // May be called on any thread until the first time the pointer is used.
  base::WeakPtr<RequestJob> GetWeakPtr() {
    return weak_ptr_factory_.GetWeakPtr();
  }

  void Start() {
    request_ = session_->context->CreateRequest(
        url_, options_.priority, this, kTrafficAnnotation,
        net::handles::kInvalidNetworkHandle);
    request_->set_method(options_.method);
    // A Session does not hold cookies or credentials yet. Without this, cookies
    // from one request would be sent on later ones.
    request_->set_disallow_credentials();
    request_->SetExtraRequestHeaders(request_headers_);
    request_->SetPriorityIncremental(options_.priority_incremental);
    request_->SetReferrer(options_.referrer.spec());
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

  void Cancel() { Finish(net::ERR_ABORTED); }

  // net::URLRequest::Delegate:
  int OnConnected(net::URLRequest* request,
                  const net::TransportInfo& info,
                  net::CompletionOnceCallback callback) override {
    connect_timer_.Stop();
    StartReadTimer();
    return net::OK;
  }

  void OnReceivedRedirect(net::URLRequest* request,
                          const net::RedirectInfo& redirect_info,
                          bool* defer_redirect) override {
    // The next hop has its own connection to get.
    read_timer_.Stop();
    StartConnectTimer();
    RequestEvent event;
    event.type = MITTO_EVENT_REDIRECT_FOLLOWED;
    event.status = redirect_info.status_code;
    event.url = redirect_info.new_url.spec();
    event.headers = HeaderLines(*request->response_headers());
    channel_->Push(std::move(event));
    session_->ready_requests.Add(handle_);
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
    RequestEvent event;
    event.type = MITTO_EVENT_RESPONSE_STARTED;
    event.status = request->GetResponseCode();
    event.protocol = net::HttpConnectionInfoToString(
        request->response_info().connection_info);
    event.url = request->url().spec();
    event.headers = HeaderLines(*request->response_headers());
    channel_->Push(std::move(event));
    session_->ready_requests.Add(handle_);

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
      read_timer_.Start(FROM_HERE, *options_.read_timeout,
                        base::BindOnce(&RequestJob::OnReadTimerFired,
                                       base::Unretained(this)));
    }
  }

  void OnReadTimerFired() {
    // A content decoder can take bytes and give nothing back yet, and then //net
    // does not complete the read, although the server is still sending.
    if (request_->GetTotalReceivedBytes() !=
        received_bytes_at_read_timer_start_) {
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
  const RequestOptions options_;
  std::unique_ptr<net::URLRequest> request_;
  bool response_started_ = false;
  bool read_wanted_ = false;
  base::OneShotTimer connect_timer_;
  base::OneShotTimer read_timer_;
  base::OneShotTimer total_timer_;
  base::ByteSize received_bytes_at_read_timer_start_;
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

// Called on the network thread.
std::unique_ptr<MittoSession> CreateSessionOnNetworkThread(
    SessionSettings settings,
    net::HostResolverManager* host_resolver_manager,
    base::TimeTicks doh_probes_start_time) {
  auto session = std::make_unique<MittoSession>();
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
  builder.DisableHttpCache();
  // Chrome's network service turns these on. They add to what //net puts in
  // Accept-Encoding, over HTTPS and to localhost only.
  builder.set_enable_brotli(true);
  builder.set_enable_zstd(true);
  builder.set_accept_language(settings.accept_language);
  builder.set_host_mapping_rules(std::move(settings.host_resolver_rules));
  // Proxies are not supported yet, so ignore the system's settings.
  builder.set_proxy_config_service(
      std::make_unique<net::ProxyConfigServiceFixed>(
          net::ProxyConfigWithAnnotation::CreateDirect()));
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
    // Failures reach the caller as return values. Logging them as well would
    // write to the stderr of a program that did not ask for it.
    logging::SetMinLogLevel(logging::LOGGING_FATAL);
    // //net reads the command line, but the arguments of the process that
    // loaded the library are not meant for it.
    base::CommandLine::Init(0, nullptr);
    InitFeatureList();
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

  std::unique_ptr<MittoSession> CreateSession(SessionSettings settings) {
    std::unique_ptr<MittoSession> session;
    RunAndWait(base::BindOnce(
        [](NetworkCore* core, SessionSettings settings,
           std::unique_ptr<MittoSession>* session) {
          *session = CreateSessionOnNetworkThread(
              std::move(settings), core->host_resolver_manager_.get(),
              core->doh_probes_start_time_);
        },
        base::Unretained(this), std::move(settings),
        base::Unretained(&session)));
    return session;
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
  *session = core.CreateSession(std::move(*parsed)).release();
  return net::OK;
}

void mitto_session_destroy(MittoSession* session) {
  if (session) {
    mitto::NetworkCore::Get().DestroySession(base::WrapUnique(session));
  }
}

int32_t mitto_session_event_fd(MittoSession* session, int32_t* fd) {
  if (!fd) {
    return net::ERR_INVALID_ARGUMENT;
  }
  *fd = -1;
  if (!session) {
    return net::ERR_INVALID_ARGUMENT;
  }
  const std::optional<int> descriptor =
      session->ready_requests.GetFileDescriptor();
  if (!descriptor) {
    return net::ERR_INSUFFICIENT_RESOURCES;
  }
  *fd = *descriptor;
  return net::OK;
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

int32_t mitto_request_start(MittoSession* session,
                            const char* url,
                            const MittoHeader* headers,
                            size_t header_count,
                            const char* options,
                            size_t options_size,
                            MittoRequest** request) {
  if (!request) {
    return net::ERR_INVALID_ARGUMENT;
  }
  *request = nullptr;
  if (!session || !url || (!headers && header_count != 0) || !options) {
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
  if (!request_headers || !parsed_options) {
    return net::ERR_INVALID_ARGUMENT;
  }

  auto handle = std::make_unique<MittoRequest>();
  handle->channel = base::MakeRefCounted<mitto::RequestChannel>();
  auto job = std::make_unique<mitto::RequestJob>(
      *session, reinterpret_cast<uintptr_t>(handle.get()), handle->channel,
      GURL(url), std::move(*request_headers), std::move(*parsed_options));
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
