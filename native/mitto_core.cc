#include <stdlib.h>

#include <memory>
#include <string>
#include <string_view>

#include "base/at_exit.h"
#include "base/check.h"
#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "base/message_loop/message_pump_type.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/string_view_util.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/threading/thread.h"
#include "mitto/mitto.h"
#include "net/base/io_buffer.h"
#include "net/base/net_errors.h"
#include "net/base/network_handle.h"
#include "net/base/request_priority.h"
#include "net/http/http_connection_info.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_response_info.h"
#include "net/proxy_resolution/proxy_config_service_fixed.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"
#include "net/ssl/ssl_info.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/url_request.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_builder.h"
#include "url/gurl.h"

namespace mitto {
namespace {

constexpr int kReadBufferSize = 64 * 1024;

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation(
        "mitto_get",
        "A GET requested by the application that loaded the library.");

// One GET. The calling thread owns the job and blocks in Wait() while the
// network thread runs the request. The results may be read once Wait() returns.
class GetJob final : public net::URLRequest::Delegate {
 public:
  explicit GetJob(const char* url)
      : url_(url),
        read_buffer_(
            base::MakeRefCounted<net::IOBufferWithSize>(kReadBufferSize)) {}

  GetJob(const GetJob&) = delete;
  GetJob& operator=(const GetJob&) = delete;

  ~GetJob() override = default;

  // Called on the network thread.
  void Start(const net::URLRequestContext& context) {
    request_ = context.CreateRequest(url_, net::DEFAULT_PRIORITY, this,
                                     kTrafficAnnotation,
                                     net::handles::kInvalidNetworkHandle);
    // Cookies and credentials belong to a Session, which does not exist yet.
    // Without this, cookies from one call would be sent on later ones.
    request_->set_disallow_credentials();
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
  const scoped_refptr<net::IOBufferWithSize> read_buffer_;
  std::unique_ptr<net::URLRequest> request_;
  base::WaitableEvent done_;

  int net_error_ = net::ERR_FAILED;
  int status_ = 0;
  std::string protocol_;
  std::string headers_;
  std::string body_;
};

// Process-wide state: what //base needs before //net can run, the network
// thread, and the request context that lives on it. It is never destroyed,
// because the library cannot know when the process that loaded it stops using
// it, so the library must not be unloaded once it has been used.
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
    base::FeatureList::InitInstance(std::string(), std::string());
    base::ThreadPoolInstance::CreateAndStartWithDefaultParams("mitto");
    CHECK(network_thread_.StartWithOptions(
        base::Thread::Options(base::MessagePumpType::IO, 0)));
    network_thread_.task_runner()->PostTask(
        FROM_HERE,
        base::BindOnce(&NetworkCore::CreateContext, base::Unretained(this)));
  }

  // Runs `job` on the network thread and blocks until it finishes.
  void Run(GetJob& job) {
    network_thread_.task_runner()->PostTask(
        FROM_HERE,
        base::BindOnce(&NetworkCore::StartJob, base::Unretained(this),
                       base::Unretained(&job)));
    job.Wait();
  }

 private:
  void CreateContext() {
    net::URLRequestContextBuilder builder;
    builder.DisableHttpCache();
    // Proxies are not supported yet, so ignore the system's settings.
    builder.set_proxy_config_service(
        std::make_unique<net::ProxyConfigServiceFixed>(
            net::ProxyConfigWithAnnotation::CreateDirect()));
    context_ = builder.Build();
  }

  void StartJob(GetJob* job) { job->Start(*context_); }

  base::AtExitManager at_exit_manager_;
  base::Thread network_thread_;
  // Used only on the network thread.
  std::unique_ptr<net::URLRequestContext> context_;
};

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

int32_t mitto_get(const char* url, MittoResponse* response) {
  if (!response) {
    return net::ERR_INVALID_ARGUMENT;
  }
  *response = {};
  if (!url) {
    return net::ERR_INVALID_ARGUMENT;
  }

  mitto::GetJob job(url);
  mitto::NetworkCore::Get().Run(job);
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
