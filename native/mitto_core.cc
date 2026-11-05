#include <stdlib.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/at_exit.h"
#include "base/check.h"
#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/memory/scoped_refptr.h"
#include "base/message_loop/message_pump_type.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/string_view_util.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/threading/thread.h"
#include "mitto/feature_list.h"
#include "mitto/ignore_errors_cert_verifier.h"
#include "mitto/mitto.h"
#include "mitto/network_session_configurator.h"
#include "mitto/session_settings.h"
#include "mitto/ssl_config_service_mojo.h"
#include "net/base/io_buffer.h"
#include "net/base/net_errors.h"
#include "net/base/network_handle.h"
#include "net/base/request_priority.h"
#include "net/cert/cert_verifier.h"
#include "net/http/http_connection_info.h"
#include "net/http/http_network_session.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_response_info.h"
#include "net/http/http_util.h"
#include "net/proxy_resolution/proxy_config_service_fixed.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"
#include "net/ssl/ssl_info.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
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

// The handle the C API hands out. It is created, used and destroyed only on the
// network thread. Other threads just hold the pointer.
struct MittoSession {
  MittoSession();
  ~MittoSession();

  std::unique_ptr<net::URLRequestContext> context;
};

MittoSession::MittoSession() = default;
MittoSession::~MittoSession() = default;

namespace mitto {
namespace {

constexpr int kReadBufferSize = 64 * 1024;

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation(
        "mitto_session_request",
        "A GET requested by the application that loaded the library.");

// One GET. The calling thread owns the job and blocks in Wait() while the
// network thread runs the request. The results may be read once Wait() returns.
class RequestJob final : public net::URLRequest::Delegate {
 public:
  // An empty `referrer` means that the request has none.
  RequestJob(const char* url,
             net::HttpRequestHeaders headers,
             net::RequestPriority priority,
             bool priority_incremental,
             GURL referrer)
      : url_(url),
        request_headers_(std::move(headers)),
        priority_(priority),
        priority_incremental_(priority_incremental),
        referrer_(std::move(referrer)),
        read_buffer_(
            base::MakeRefCounted<net::IOBufferWithSize>(kReadBufferSize)) {}

  RequestJob(const RequestJob&) = delete;
  RequestJob& operator=(const RequestJob&) = delete;

  ~RequestJob() override = default;

  // Called on the network thread.
  void Start(const net::URLRequestContext& context) {
    request_ = context.CreateRequest(url_, priority_, this, kTrafficAnnotation,
                                     net::handles::kInvalidNetworkHandle);
    // A Session does not hold cookies or credentials yet. Without this, cookies
    // from one request would be sent on later ones.
    request_->set_disallow_credentials();
    request_->SetExtraRequestHeaders(request_headers_);
    request_->SetPriorityIncremental(priority_incremental_);
    request_->SetReferrer(referrer_.spec());
    request_->Start();
  }

  void Wait() { done_.Wait(); }

  int net_error() const { return net_error_; }
  int status() const { return status_; }
  std::string_view protocol() const { return protocol_; }
  std::string_view headers() const { return headers_; }
  std::string_view body() const { return body_; }

  // net::URLRequest::Delegate:
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
    ReadBody();
  }

  void OnReadCompleted(net::URLRequest* request, int bytes_read) override {
    if (bytes_read <= 0) {
      Finish(bytes_read);
      return;
    }
    AppendToBody(bytes_read);
    ReadBody();
  }

 private:
  // Reads until the request has to wait for the network, which continues in
  // OnReadCompleted(), or until the body ends.
  void ReadBody() {
    while (true) {
      const int result = request_->Read(read_buffer_.get(), kReadBufferSize);
      if (result == net::ERR_IO_PENDING) {
        return;
      }
      if (result <= 0) {
        Finish(result);
        return;
      }
      AppendToBody(result);
    }
  }

  void AppendToBody(int size) {
    body_.append(
        base::as_string_view(read_buffer_->first(static_cast<size_t>(size))));
  }

  // `result` is net::OK when the body ended, otherwise a net error.
  void Finish(int result) {
    if (result == net::OK) {
      status_ = request_->GetResponseCode();
      protocol_ = net::HttpConnectionInfoToString(
          request_->response_info().connection_info);
      size_t iterator = 0;
      std::string name;
      std::string value;
      while (request_->response_headers()->EnumerateHeaderLines(
          &iterator, &name, &value)) {
        base::StrAppend(&headers_, {name, ": ", value, "\r\n"});
      }
    }
    net_error_ = result;
    // Deleting the request from its delegate's callback is allowed. The signal
    // has to come last, because the calling thread may delete this job after
    // it.
    request_.reset();
    done_.Signal();
  }

  const GURL url_;
  const net::HttpRequestHeaders request_headers_;
  const net::RequestPriority priority_;
  const bool priority_incremental_;
  const GURL referrer_;
  const scoped_refptr<net::IOBufferWithSize> read_buffer_;
  std::unique_ptr<net::URLRequest> request_;
  base::WaitableEvent done_;

  int net_error_ = net::ERR_FAILED;
  int status_ = 0;
  std::string protocol_;
  std::string headers_;
  std::string body_;
};

// Called on the network thread.
std::unique_ptr<MittoSession> CreateSessionOnNetworkThread(
    SessionSettings settings) {
  net::URLRequestContextBuilder builder;
  builder.DisableHttpCache();
  // Chrome's network service turns these on. They add to what //net puts in
  // Accept-Encoding, over HTTPS and to localhost only.
  builder.set_enable_brotli(true);
  builder.set_enable_zstd(true);
  builder.set_accept_language(settings.accept_language);
  // Proxies are not supported yet, so ignore the system's settings.
  builder.set_proxy_config_service(
      std::make_unique<net::ProxyConfigServiceFixed>(
          net::ProxyConfigWithAnnotation::CreateDirect()));
  if (!settings.ignore_certificate_errors_spki_list.empty()) {
    builder.SetCertVerifier(std::make_unique<IgnoreErrorsCertVerifier>(
        net::CertVerifier::CreateDefault(/*cert_net_fetcher=*/nullptr),
        std::move(settings.ignore_certificate_errors_spki_list)));
  }
  builder.set_ssl_config_service(
      std::make_unique<SSLConfigServiceMojo>(GetNewSSLConfig()));

  net::HttpNetworkSessionParams session_params;
  ConfigureHttp2Params(&session_params);
  builder.set_http_network_session_params(session_params);

  auto session = std::make_unique<MittoSession>();
  session->context = builder.Build();
  return session;
}

// Process-wide state: what //base needs before //net can run, and the network
// thread that all Sessions live on. It is never destroyed, because the library
// cannot know when the process that loaded it stops using it. That is why the
// library is built to stay loaded until the process exits.
class NetworkCore {
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
  }

  std::unique_ptr<MittoSession> CreateSession(SessionSettings settings) {
    std::unique_ptr<MittoSession> session;
    RunAndWait(base::BindOnce(
        [](SessionSettings settings, std::unique_ptr<MittoSession>* session) {
          *session = CreateSessionOnNetworkThread(std::move(settings));
        },
        std::move(settings), base::Unretained(&session)));
    return session;
  }

  void DestroySession(std::unique_ptr<MittoSession> session) {
    RunAndWait(base::BindOnce([](std::unique_ptr<MittoSession> session) {},
                              std::move(session)));
  }

  // Runs `job` on the network thread and blocks until it finishes.
  void RunRequest(MittoSession& session, RequestJob& job) {
    network_thread_.task_runner()->PostTask(
        FROM_HERE,
        base::BindOnce([](MittoSession* session,
                          RequestJob* job) { job->Start(*session->context); },
                       base::Unretained(&session), base::Unretained(&job)));
    job.Wait();
  }

 private:
  void RunAndWait(base::OnceClosure task) {
    base::WaitableEvent done;
    network_thread_.task_runner()->PostTask(
        FROM_HERE, std::move(task).Then(base::BindOnce(
                       &base::WaitableEvent::Signal, base::Unretained(&done))));
    done.Wait();
  }

  base::AtExitManager at_exit_manager_;
  base::Thread network_thread_;
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

int32_t mitto_session_request(MittoSession* session,
                              const char* url,
                              const MittoHeader* headers,
                              size_t header_count,
                              int32_t priority,
                              bool priority_incremental,
                              const char* referrer,
                              MittoResponse* response) {
  if (!response) {
    return net::ERR_INVALID_ARGUMENT;
  }
  *response = {};
  if (!session || !url || (!headers && header_count != 0) ||
      priority < net::MINIMUM_PRIORITY || priority > net::MAXIMUM_PRIORITY) {
    return net::ERR_INVALID_ARGUMENT;
  }
  // SAFETY: The C API says `headers` holds `header_count` elements. A span can
  // be null only if it is empty, which the check above makes sure of.
  std::optional<net::HttpRequestHeaders> request_headers =
      mitto::ToRequestHeaders(
          UNSAFE_BUFFERS(base::span(headers, header_count)));
  if (!request_headers) {
    return net::ERR_INVALID_ARGUMENT;
  }
  // //net would silently send no Referer for a URL that it cannot send as one.
  GURL referrer_url;
  if (referrer) {
    referrer_url = GURL(referrer).GetAsReferrer();
    if (!referrer_url.is_valid()) {
      return net::ERR_INVALID_ARGUMENT;
    }
  }

  mitto::RequestJob job(url, std::move(*request_headers),
                        static_cast<net::RequestPriority>(priority),
                        priority_incremental, std::move(referrer_url));
  mitto::NetworkCore::Get().RunRequest(*session, job);
  if (job.net_error() != net::OK) {
    return job.net_error();
  }

  response->status = job.status();
  response->protocol = mitto::CopyToNewBuffer(job.protocol());
  response->headers = mitto::CopyToNewBuffer(job.headers());
  response->headers_size = job.headers().size();
  response->body =
      reinterpret_cast<uint8_t*>(mitto::CopyToNewBuffer(job.body()));
  response->body_size = job.body().size();
  return net::OK;
}

void mitto_response_free(MittoResponse* response) {
  if (!response) {
    return;
  }
  free(response->protocol);
  free(response->headers);
  free(response->body);
  *response = {};
}

}  // extern "C"
