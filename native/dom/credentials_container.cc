#include "mitto/dom/credentials_container.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "base/base64url.h"
#include "base/check.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_messages.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/runtime_features.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_credentials_container.h"
#include "mitto/bindings/v8_window.h"
#include "mitto/dom/abort_signal.h"
#include "mitto/dom/document.h"
#include "mitto/dom/window.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "net/base/url_util.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "url/url_canon.h"
#include "url/url_canon_stdstring.h"
#include "url/url_constants.h"
#include "url/url_util.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-json.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-traced-handle.h"

namespace mitto {

namespace {

// kWebOTPRequestTimeout: how long the browser waits for a text.
constexpr base::TimeDelta kOtpTimeout = base::Minutes(4);
// kPublicKeyCredentialDescriptorListMaxSize of authenticator.mojom.
constexpr size_t kMaxAllowCredentials = 64;
// kMaxLargeBlobSize.
constexpr size_t kMaxLargeBlobSize = 2048;
// device::kMaxPRFInputSize.
constexpr size_t kMaxPrfInputSize = 256;
// FedCM's limit on providers in one request.
constexpr size_t kMaxIdentityProviders = 10;

// The AppIDs that pages of google.com may claim besides their own, as
// WebAuthRequestSecurityCheckerImpl lists them.
constexpr char kGstaticAppId[] =
    "https://www.gstatic.com/securitykey/origins.json";
constexpr char kGstaticCorpAppId[] =
    "https://www.gstatic.com/securitykey/a/google.com/origins.json";

// What the browser's answer does, in the context of the page that asked.
using Answer = base::OnceCallback<void(v8::Isolate*, v8::Local<v8::Context>)>;

void RunAnswer(v8::Isolate* isolate,
               v8::Global<v8::Context> context_handle,
               Answer answer) {
  const v8::HandleScope handle_scope(isolate);
  const v8::Local<v8::Context> context = context_handle.Get(isolate);
  const v8::Context::Scope context_scope(context);
  // A window that has gone gets no answer, as Blink's resolvers settle nothing
  // once their execution context is destroyed.
  const Window* const window =
      bindings::V8Window::ToWrappable(isolate, context->Global());
  if (!window || !window->HasContext()) {
    return;
  }
  std::move(answer).Run(isolate, context);
}

// Runs `answer` in `context` from a task of its own, after `delay`, as the
// browser's answer comes over mojo.
void PostAnswer(v8::Isolate* isolate,
                v8::Local<v8::Context> context,
                Answer answer,
                base::TimeDelta delay = base::TimeDelta()) {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&RunAnswer, base::Unretained(isolate),
                     v8::Global<v8::Context>(isolate, context),
                     std::move(answer)),
      delay);
}

v8::Local<v8::Value> NewDOMException(v8::Isolate* isolate,
                                     std::string_view name,
                                     std::string_view message) {
  return bindings::PerIsolateData::From(isolate)->dom_exception_hooks().create(
      isolate, name, message);
}

// Rejects at once, as Blink's resolver->Reject() of a DOMException does, with
// no context in front of the message.
void RejectNow(v8::Isolate* isolate,
               v8::Local<v8::Promise::Resolver> resolver,
               std::string_view name,
               std::string_view message) {
  std::ignore = resolver->Reject(isolate->GetCurrentContext(),
                                 NewDOMException(isolate, name, message));
}

// ScriptPromiseResolver::RejectWithTypeError(), whose resolver has get()'s
// exception context.
void RejectWithTypeErrorNow(v8::Isolate* isolate,
                            v8::Local<v8::Promise::Resolver> resolver,
                            std::string_view message) {
  std::ignore = resolver->Reject(
      isolate->GetCurrentContext(),
      v8::Exception::TypeError(bindings::ToV8StringFromUtf8(
          isolate, bindings::ExceptionMessages::FailedToExecute(
                       "get", "CredentialsContainer", message))));
}

bool IsEnabled(v8::Isolate* isolate, bindings::RuntimeFeature feature) {
  return bindings::PerIsolateData::From(isolate)->features().IsEnabled(feature);
}

url::Origin DocumentOrigin() {
  return url::Origin::Create(
      GURL(base::UTF16ToUTF8(Window::Current().GetDocument().UrlForBinding())));
}

// The bytes of a BufferSource, as AsSpan<SharedBufferPolicy::kDisallow>()
// reads them: none for a buffer that has been detached.
std::vector<uint8_t> BytesOf(
    const bindings::ArrayBufferOrArrayBufferView& source) {
  if (const auto* buffer = std::get_if<v8::Local<v8::ArrayBuffer>>(&source)) {
    if ((*buffer)->WasDetached() || !(*buffer)->ByteLength()) {
      return {};
    }
    // SAFETY: V8 gives the buffer's data and its length in bytes.
    const base::span<const uint8_t> bytes = UNSAFE_BUFFERS(
        base::span(static_cast<const uint8_t*>((*buffer)->Data()),
                   (*buffer)->ByteLength()));
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
  }
  const v8::Local<v8::ArrayBufferView> view =
      std::get<v8::Local<v8::ArrayBufferView>>(source);
  std::vector<uint8_t> bytes(view->ByteLength());
  bytes.resize(view->CopyContents(bytes.data(), bytes.size()));
  return bytes;
}

size_t SizeOf(const bindings::ArrayBufferOrArrayBufferView& source) {
  if (const auto* buffer = std::get_if<v8::Local<v8::ArrayBuffer>>(&source)) {
    return (*buffer)->WasDetached() ? 0 : (*buffer)->ByteLength();
  }
  return std::get<v8::Local<v8::ArrayBufferView>>(source)->ByteLength();
}

// validatePRFInputs().
const char* CheckPrfValues(
    const bindings::AuthenticationExtensionsPRFValues& values) {
  if ((values.first && SizeOf(*values.first) > kMaxPrfInputSize) ||
      (values.second && SizeOf(*values.second) > kMaxPrfInputSize)) {
    return "'prf' extension contains excessively large input";
  }
  return nullptr;
}

// validateGetPublicKeyCredentialPRFExtension(): its message for a SyntaxError,
// or none.
const char* CheckPrfExtension(
    const bindings::AuthenticationExtensionsPRFInputs& prf,
    const std::vector<bindings::PublicKeyCredentialDescriptor>&
        allow_credentials) {
  std::vector<std::vector<uint8_t>> ids;
  for (const bindings::PublicKeyCredentialDescriptor& descriptor :
       allow_credentials) {
    ids.push_back(descriptor.id ? BytesOf(*descriptor.id)
                                : std::vector<uint8_t>());
  }
  if (prf.eval) {
    if (const char* error = CheckPrfValues(*prf.eval)) {
      return error;
    }
  }
  if (!prf.eval_by_credential) {
    return nullptr;
  }
  for (const auto& [key, values] : *prf.eval_by_credential) {
    // Base64UnpaddedUrlDecode(): base64url with no padding, of Latin-1.
    std::string decoded;
    const bool latin1 = std::ranges::all_of(
        key, [](char16_t character) { return character < 0x100; });
    const std::string encoded = base::UTF16ToUTF8(key);
    if (!latin1 || encoded.find_first_of("+/=") != std::string::npos ||
        !base::Base64UrlDecode(
            encoded, base::Base64UrlDecodePolicy::IGNORE_PADDING, &decoded)) {
      return "'prf' extension contains invalid base64url data in "
             "'evalByCredential'";
    }
    if (decoded.empty()) {
      return "'prf' extension contains an empty credential ID in "
             "'evalByCredential'";
    }
    const std::vector<uint8_t> id(decoded.begin(), decoded.end());
    if (std::ranges::find(ids, id) == ids.end()) {
      return "'prf' extension contains 'evalByCredential' key that doesn't "
             "match any in allowedCredentials";
    }
    if (const char* error = CheckPrfValues(values)) {
      return error;
    }
  }
  return nullptr;
}

// webauthn::OriginIsAllowedToClaimRelyingPartyId() for an origin that may make
// WebAuthn requests.
bool MayClaimRelyingPartyId(const std::string& rp_id,
                            const url::Origin& origin) {
  namespace registry = net::registry_controlled_domains;
  if (rp_id.empty()) {
    return false;
  }
  if (origin.host() == rp_id) {
    return true;
  }
  if (!origin.DomainIs(rp_id) ||
      !registry::HostHasRegistryControlledDomain(
          rp_id, registry::INCLUDE_UNKNOWN_REGISTRIES,
          registry::INCLUDE_PRIVATE_REGISTRIES)) {
    return false;
  }
  // kRejectRpIdsInsideCallersPublicSuffix, on by default: the RP ID must be
  // longer than the caller's public suffix.
  const std::optional<std::string_view> caller_registry = registry::GetRegistry(
      origin.GetURL(), registry::INCLUDE_UNKNOWN_REGISTRIES,
      registry::INCLUDE_PRIVATE_REGISTRIES);
  return caller_registry && !caller_registry->empty() &&
         rp_id.size() > caller_registry->size();
}

// Whether webauthn::GetRemoteValidationUrl() has a URL for the RP ID: one that
// looks like a host name, which a related origin request may then fetch.
bool HasRemoteValidationUrl(const std::string& rp_id) {
  std::string canonical;
  url::StdStringCanonOutput output(&canonical);
  url::CanonHostInfo host_info;
  url::CanonicalizeHostVerbose(std::string_view(rp_id),
                               url::Component(0, rp_id.size()), &output,
                               &host_info);
  output.Complete();
  return host_info.family == url::CanonHostInfo::NEUTRAL &&
         net::IsCanonicalizedHostCompliant(canonical);
}

// WebAuthRequestSecurityCheckerImpl::ValidateAppIdExtension(): whether the
// origin may claim the AppID of the U2F extension.
bool MayClaimAppId(std::string appid, const url::Origin& origin) {
  if (appid.empty()) {
    appid = origin.Serialize();
  }
  const GURL appid_url(appid);
  if (!appid_url.is_valid() || appid_url.scheme() != url::kHttpsScheme ||
      appid_url.scheme() != origin.scheme()) {
    return false;
  }
  if (appid_url.host() == origin.host() ||
      net::registry_controlled_domains::SameDomainOrHost(
          appid_url, origin,
          net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES)) {
    return true;
  }
  return origin.DomainIs("google.com") && !appid_url.has_ref() &&
         (appid_url.EqualsIgnoringRef(GURL(kGstaticAppId)) ||
          appid_url.EqualsIgnoringRef(GURL(kGstaticCorpAppId)));
}

// What the browser's authenticator answers a WebAuthn request at once, before
// it shows its dialog, as an AuthenticatorStatus that Blink turns into a
// DOMException: its name and message, or none for a request that goes on to
// the dialog.
struct Refusal {
  std::string name;
  std::string message;
};

std::optional<Refusal> AuthenticatorRefusal(
    const url::Origin& origin,
    const bindings::PublicKeyCredentialRequestOptions& options,
    bool* waits_on_related_origins) {
  // webauthn::OriginAllowedToMakeWebAuthnRequests(): the document of a
  // credentials container is a secure context, so only the scheme and an IP
  // address are left to check.
  if (origin.opaque()) {
    return Refusal{"NotAllowedError",
                   "The current origin is an opaque origin and hence not "
                   "allowed to access 'PublicKeyCredential' objects."};
  }
  if (origin.scheme() != url::kHttpScheme &&
      origin.scheme() != url::kHttpsScheme) {
    return Refusal{
        "SecurityError",
        "Public-key credentials are only available to HTTPS origins with "
        "valid certificates, HTTP origins that fall under 'localhost', or "
        "pages served from an extension. See "
        "https://chromium.googlesource.com/chromium/src/+/main/content/"
        "browser/webauth/origins.md for details"};
  }
  if (url::HostIsIPAddress(origin.host())) {
    return Refusal{"SecurityError", "This is an invalid domain."};
  }
  // MojoPublicKeyCredentialRequestOptions: the document's domain for no RP ID.
  const std::string rp_id =
      options.rp_id ? base::UTF16ToUTF8(*options.rp_id) : origin.host();
  if (!MayClaimRelyingPartyId(rp_id, origin)) {
    if (!HasRemoteValidationUrl(rp_id)) {
      return Refusal{"SecurityError",
                     "The relying party ID is not a registrable domain suffix "
                     "of, nor equal to the current domain."};
    }
    // A related origin request fetches https://<RP ID>/.well-known/webauthn,
    // which the Runtime does not do: the request waits, as it does behind the
    // dialog that follows when the file lists the document's origin.
    *waits_on_related_origins = true;
    return std::nullopt;
  }
  if (options.extensions && options.extensions->appid &&
      !MayClaimAppId(base::UTF16ToUTF8(*options.extensions->appid), origin)) {
    return Refusal{"SecurityError", "This is an invalid domain."};
  }
  return std::nullopt;
}

}  // namespace

// A request that waits on the browser until it answers or the request's signal
// aborts it: the resolver and Blink's ScopedAbortState, its signal and the
// algorithm that aborts it.
class CredentialsContainer::Request final
    : public cppgc::GarbageCollected<Request> {
 public:
  Request(v8::Isolate* isolate,
          v8::Local<v8::Promise::Resolver> resolver,
          AbortSignal* signal,
          RequestKind kind)
      : resolver_(isolate, resolver), signal_(signal), kind_(kind) {}

  void Trace(cppgc::Visitor* visitor) const {
    visitor->Trace(resolver_);
    visitor->Trace(signal_);
    visitor->Trace(algorithm_);
  }

  v8::Local<v8::Promise::Resolver> resolver(v8::Isolate* isolate) const {
    return resolver_.Get(isolate);
  }
  AbortSignal* signal() const { return signal_.Get(); }
  void set_algorithm(AbortSignal::Algorithm* algorithm) {
    algorithm_ = algorithm;
  }
  RequestKind kind() const { return kind_; }

  // Whether a FedCM request waits on the browser, which only an abort ends.
  bool waiting() const { return waiting_; }
  void set_waiting(bool waiting) { waiting_ = waiting; }
  // Whether the browser's answer has come, which takes the request's
  // algorithm off its signal, as Blink's ScopedAbortState does once the
  // callback has it, so that a signal that outlives the request does not hold
  // it.
  bool answered() const { return answered_; }
  void set_answered() {
    answered_ = true;
    if (signal_ && algorithm_) {
      signal_->RemoveAlgorithm(algorithm_.Get());
    }
    algorithm_ = nullptr;
  }

 private:
  v8::TracedReference<v8::Promise::Resolver> resolver_;
  const cppgc::Member<AbortSignal> signal_;
  cppgc::Member<AbortSignal::Algorithm> algorithm_;
  const RequestKind kind_;
  bool waiting_ = false;
  bool answered_ = false;
};

// PublicKeyRequestAbortAlgorithm, OtpRequestAbortAlgorithm and
// FedCmRequestAbortAlgorithm: each tells the browser to drop its request.
class CredentialsContainer::AbortRequest final : public AbortSignal::Algorithm {
 public:
  AbortRequest(CredentialsContainer* container, Request* request)
      : container_(container), request_(request) {}

  void Run() override { container_->Abort(request_.Get()); }

  void Trace(cppgc::Visitor* visitor) const override {
    AbortSignal::Algorithm::Trace(visitor);
    visitor->Trace(container_);
    visitor->Trace(request_);
  }

 private:
  const cppgc::Member<CredentialsContainer> container_;
  const cppgc::Member<Request> request_;
};

CredentialsContainer* CredentialsContainer::Create() {
  return bindings::MakeGarbageCollected<CredentialsContainer>();
}

CredentialsContainer::CredentialsContainer() = default;
CredentialsContainer::~CredentialsContainer() = default;

const bindings::WrapperTypeInfo* CredentialsContainer::GetWrapperTypeInfo()
    const {
  return &bindings::V8CredentialsContainer::kWrapperTypeInfo;
}

void CredentialsContainer::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(public_key_request_);
  visitor->Trace(otp_request_);
}

v8::Local<v8::Promise> CredentialsContainer::Get(
    const bindings::CredentialRequestOptions& options,
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return {};
  }
  const v8::Local<v8::Promise> promise = resolver->GetPromise();
  AbortSignal* const signal = options.signal ? *options.signal : nullptr;
  if (signal && signal->Aborted()) {
    std::ignore = resolver->Reject(context, signal->Reason());
    return promise;
  }
  // HasCredentialTypeInRequest(). A member that a runtime-enabled feature
  // holds, such as `digital`, converts only where the Page has the feature.
  if (!options.federated && !options.identity && !options.password &&
      !options.otp && !options.public_key && !options.digital) {
    RejectNow(isolate, resolver, "NotSupportedError",
              "No credential type was specified in the request.");
    return promise;
  }
  if (options.digital) {
    GetDigital(isolate, resolver, *options.digital);
    return promise;
  }
  // CheckSecurityRequirementsBeforeRequest() passes: the document is a
  // top-level one, same-origin with all of its ancestors, and its permissions
  // policy, which no Permissions-Policy header narrows, allows each feature
  // that the request needs to its own origin.
  if (options.public_key) {
    GetPublicKey(isolate, resolver, options);
    return promise;
  }
  if (options.otp) {
    // The transports' default, an empty list, has no SMS either.
    if (std::ranges::find(options.otp->transport, u"sms") ==
        options.otp->transport.end()) {
      RejectNow(isolate, resolver, "NotSupportedError",
                "Unsupported transport type for OTP Credentials");
      return promise;
    }
    GetOtp(isolate, resolver, signal);
    return promise;
  }
  if (options.identity && options.identity->providers) {
    GetIdentity(isolate, resolver, options);
    return promise;
  }
  if (options.mediation == u"conditional") {
    RejectNow(isolate, resolver, "NotSupportedError",
              "Conditional mediation is not supported for this credential "
              "type");
    return promise;
  }
  if (options.ui_mode == u"immediate") {
    if (options.password) {
      // ForwardRequestToAuthenticator() with immediate mediation, which needs
      // a user's activation.
      RejectNow(isolate, resolver, "NotAllowedError",
                "A user activation is required to request immediate "
                "credentials.");
    } else {
      RejectNow(isolate, resolver, "NotSupportedError",
                "Immediate uiMode is not supported for this credential type");
    }
    return promise;
  }
  GetFromPasswordStore(isolate, resolver);
  return promise;
}

void CredentialsContainer::GetDigital(
    v8::Isolate* isolate,
    v8::Local<v8::Promise::Resolver> resolver,
    const bindings::DigitalCredentialRequestOptions& digital) {
  // DiscoverDigitalIdentityCredentialFromExternalSource(). The
  // 'digital-credentials-get' policy allows the document's own origin.
  if (DocumentOrigin().opaque()) {
    RejectNow(isolate, resolver, "NotAllowedError",
              "The credential operation is not allowed in an opaque origin.");
    return;
  }
  // FilterSupportedRequests(): every protocol, unless the Page has
  // DigitalCredentialsProtocolFilter.
  const bool filter = IsEnabled(
      isolate, bindings::RuntimeFeature::kDigitalCredentialsProtocolFilter);
  static constexpr std::u16string_view kPresentationProtocols[] = {
      u"openid4vp-v1-unsigned", u"openid4vp-v1-signed",
      u"openid4vp-v1-multisigned", u"org-iso-mdoc"};
  const bool any_request = std::ranges::any_of(
      *digital.requests,
      [filter](const bindings::DigitalCredentialGetRequest& request) {
        return !filter ||
               std::ranges::find(kPresentationProtocols, *request.protocol) !=
                   std::end(kPresentationProtocols);
      });
  if (!any_request) {
    RejectWithTypeErrorNow(isolate, resolver,
                           "Digital Credentials API call with no well-formed "
                           "allowed protocol requests.");
    return;
  }
  // CheckUserActivationOrReject(): a Page has no user activation, nor a
  // delegated capability.
  RejectNow(
      isolate, resolver, "NotAllowedError",
      IsEnabled(
          isolate,
          bindings::RuntimeFeature::kCapabilityDelegationDigitalCredentials)
          ? "The 'digital-credentials-get' feature requires transient "
            "activation or delegated capability."
          : "The 'digital-credentials-get' feature requires transient "
            "activation.");
}

void CredentialsContainer::GetPublicKey(
    v8::Isolate* isolate,
    v8::Local<v8::Promise::Resolver> resolver,
    const bindings::CredentialRequestOptions& options) {
  const bindings::PublicKeyCredentialRequestOptions& public_key =
      *options.public_key;
  const bindings::AuthenticationExtensionsClientInputs* const extensions =
      public_key.extensions ? &*public_key.extensions : nullptr;
  const bool conditional = options.mediation == u"conditional";
  const bool immediate = options.ui_mode == u"immediate";
  if (conditional && extensions && extensions->remote_client_data_js_on) {
    RejectNow(isolate, resolver, "NotAllowedError",
              "Conditional mediation cannot be used with a "
              "remoteClientDataJSON request.");
    return;
  }
  // Ambient mediation needs WebAuthenticationAmbient, which no Persona has.
  if (conditional && immediate) {
    RejectNow(isolate, resolver, "NotSupportedError",
              "Immediate uiMode is not compatible with conditional mediation");
    return;
  }
  if (immediate) {
    if (!public_key.allow_credentials.empty()) {
      RejectNow(isolate, resolver, "NotAllowedError",
                "An allowCredentials is not allowed with immediate mediation.");
      return;
    }
    if (extensions && extensions->remote_desktop_client_override) {
      RejectNow(isolate, resolver, "NotAllowedError",
                "Immediate mediation cannot be used with a remote desktop "
                "override request.");
      return;
    }
    if (extensions && extensions->remote_client_data_js_on) {
      RejectNow(isolate, resolver, "NotAllowedError",
                "Immediate mediation cannot be used with a "
                "remoteClientDataJSON request.");
      return;
    }
    RejectNow(isolate, resolver, "NotAllowedError",
              "A user activation is required to request immediate "
              "credentials.");
    return;
  }
  // The challenge's size limit, that of a wtf_size_t, is above what a
  // BufferSource converts.
  if (public_key.allow_credentials.size() > kMaxAllowCredentials) {
    RejectNow(isolate, resolver, "RangeError",
              "The `allowCredentials` attribute exceeds the maximum allowed "
              "size (64).");
    return;
  }
  if (extensions) {
    if (extensions->appid && !extensions->appid->empty() &&
        !GURL(base::UTF16ToUTF8(*extensions->appid)).is_valid()) {
      RejectNow(isolate, resolver, "SyntaxError",
                "The `appid` extension value is neither empty/null nor a "
                "valid URL");
      return;
    }
    if (extensions->cred_props) {
      RejectNow(isolate, resolver, "NotSupportedError",
                "The 'credProps' extension is only valid when creating a "
                "credential");
      return;
    }
    if (extensions->large_blob) {
      if (extensions->large_blob->support) {
        RejectNow(isolate, resolver, "NotSupportedError",
                  "The 'largeBlob' extension's 'support' parameter is only "
                  "valid when creating a credential");
        return;
      }
      if (extensions->large_blob->write &&
          SizeOf(*extensions->large_blob->write) > kMaxLargeBlobSize) {
        RejectNow(isolate, resolver, "NotSupportedError",
                  "The 'largeBlob' extension's 'write' parameter exceeds the "
                  "maximum allowed size (2kb)");
        return;
      }
    }
    if (extensions->prf) {
      if (extensions->prf->eval_by_credential &&
          public_key.allow_credentials.empty()) {
        RejectNow(isolate, resolver, "NotSupportedError",
                  "'prf' extension has 'evalByCredential' with an empty allow "
                  "list");
        return;
      }
      if (const char* error = CheckPrfExtension(*extensions->prf,
                                                public_key.allow_credentials)) {
        RejectNow(isolate, resolver, "SyntaxError", error);
        return;
      }
    }
    // The member converts only where the Page has SecurePaymentConfirmation.
    if (extensions->payment) {
      RejectNow(isolate, resolver, "NotAllowedError",
                "The 'payment' extension is only valid when creating a "
                "credential");
      return;
    }
  }
  Request* const request =
      Wait(isolate, resolver, options.signal ? *options.signal : nullptr,
           RequestKind::kPublicKey);
  // AuthenticatorCommonImpl::GetCredential(): one request at a time.
  if (public_key_request_) {
    Refuse(request, "OperationError", "A request is already pending.");
    return;
  }
  bool waits_on_related_origins = false;
  if (std::optional<Refusal> refusal = AuthenticatorRefusal(
          DocumentOrigin(), public_key, &waits_on_related_origins)) {
    Refuse(request, std::move(refusal->name), std::move(refusal->message));
    return;
  }
  // The dialog, or the autofill of a conditional request, waits for a person,
  // who never answers. A modal request that times out shows that, and waits
  // for them to dismiss it (DoesBlockRequestOnFailure()), so it waits all the
  // same, until an abort cancels it.
  public_key_request_ = request;
}

void CredentialsContainer::GetOtp(v8::Isolate* isolate,
                                  v8::Local<v8::Promise::Resolver> resolver,
                                  AbortSignal* signal) {
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  // WebOTPService::Receive() cancels the request it held.
  if (Request* const previous = otp_request_.Get()) {
    otp_request_.Clear();
    Refuse(previous, "AbortError", "OTP retrieval was cancelled.",
           /*abort_reason_first=*/false);
  }
  Request* const request = Wait(isolate, resolver, signal, RequestKind::kOtp);
  otp_request_ = request;
  // FetchRemoteSms() finds no phone to forward a text, kCrossDeviceFailure,
  // which leaves the rejection to the service's timer, for privacy. The
  // command line's event loop does not wait for the browser's timer.
  PostAnswer(isolate, context,
             base::BindOnce(
                 [](cppgc::WeakPersistent<CredentialsContainer> container,
                    cppgc::Persistent<Request> request, v8::Isolate* isolate,
                    v8::Local<v8::Context> context) {
                   if (container) {
                     container->OtpTimedOut(request.Get());
                   }
                 },
                 cppgc::WeakPersistent<CredentialsContainer>(this),
                 cppgc::Persistent<Request>(request)),
             kOtpTimeout);
}

void CredentialsContainer::OtpTimedOut(Request* request) {
  if (otp_request_ != request) {
    return;
  }
  otp_request_.Clear();
  request->set_answered();
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  std::ignore = request->resolver(isolate)->Reject(
      isolate->GetCurrentContext(),
      NewDOMException(isolate, "InvalidStateError",
                      "OTP retrieval timed out."));
}

void CredentialsContainer::GetIdentity(
    v8::Isolate* isolate,
    v8::Local<v8::Promise::Resolver> resolver,
    const bindings::CredentialRequestOptions& options) {
  const bindings::IdentityCredentialRequestOptions& identity =
      *options.identity;
  const std::vector<bindings::IdentityProviderRequestOptions>& providers =
      *identity.providers;
  // GetForIdentity().
  if (!IsEnabled(isolate, bindings::RuntimeFeature::kFedCm)) {
    RejectNow(isolate, resolver, "NotSupportedError",
              "FedCM is not supported.");
    return;
  }
  if (providers.empty()) {
    RejectWithTypeErrorNow(isolate, resolver,
                           "Need at least one identity provider.");
    return;
  }
  if (providers.size() > kMaxIdentityProviders) {
    RejectWithTypeErrorNow(isolate, resolver,
                           "More than 10 providers are not allowed.");
    return;
  }
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const bool registration =
      IsEnabled(isolate, bindings::RuntimeFeature::kFedCmIdPRegistration) &&
      IsEnabled(isolate,
                bindings::RuntimeFeature::kFedCmMultipleIdentityProviders);
  std::vector<GURL> config_urls;
  int provider_index = 0;
  for (const bindings::IdentityProviderRequestOptions& provider : providers) {
    // IdentityProviderRequestOptions::From() writes the custom parameters as
    // JSON, which a page's getter may throw from.
    if (provider.params) {
      const v8::TryCatch try_catch(isolate);
      if (v8::JSON::Stringify(context, *provider.params).IsEmpty()) {
        std::ignore = resolver->Reject(context, try_catch.Exception());
        return;
      }
    }
    if (registration && provider.config_url == u"any") {
      continue;
    }
    if (!provider.client_id) {
      RejectWithTypeErrorNow(isolate, resolver,
                             "Missing the provider's clientId.");
      return;
    }
    ++provider_index;
    const GURL config_url(provider.config_url
                              ? base::UTF16ToUTF8(*provider.config_url)
                              : std::string());
    if (!config_url.is_valid() || provider.client_id->empty()) {
      RejectNow(isolate, resolver, "InvalidStateError",
                base::StrCat({"Provider ", base::NumberToString(provider_index),
                              " information is incomplete."}));
      return;
    }
    // The document has no Content-Security-Policy to refuse the URL.
    config_urls.push_back(config_url);
  }
  const std::u16string& mediation = options.mediation;
  if (mediation == u"conditional" &&
      !IsEnabled(isolate, bindings::RuntimeFeature::kFedCmAutofill)) {
    RejectNow(isolate, resolver, "NotSupportedError",
              "Conditional mediation is not supported for this credential "
              "type");
    return;
  }
  const bool active = identity.mode == u"active";
  if (active) {
    if (!IsEnabled(isolate, bindings::RuntimeFeature::
                                kFedCmActiveModeMultipleIdentityProviders) &&
        config_urls.size() > 1) {
      RejectNow(isolate, resolver, "InvalidStateError",
                "Active mode is not currently supported with multiple "
                "identity providers.");
      return;
    }
    if (mediation == u"silent") {
      RejectNow(isolate, resolver, "NotSupportedError",
                "mediation:silent is not supported in active mode");
      return;
    }
  }
  Request* const request =
      Wait(isolate, resolver, options.signal ? *options.signal : nullptr,
           RequestKind::kIdentity);
  // RequestHandler::RequestToken(): what the browser refuses before it asks
  // the identity providers, with a NetworkError.
  bool refused = active || DocumentOrigin().opaque();
  for (size_t index = 0; index < config_urls.size() && !refused; ++index) {
    // A provider twice, or one whose origin is not potentially trustworthy:
    // https, or http on localhost.
    const url::Origin provider_origin = url::Origin::Create(config_urls[index]);
    refused = std::ranges::count(config_urls, config_urls[index]) > 1 ||
              !(provider_origin.scheme() == url::kHttpsScheme ||
                (provider_origin.scheme() == url::kHttpScheme &&
                 net::HostStringIsLocalhost(provider_origin.host())));
  }
  if (refused) {
    // OnRequestToken() rejects with kError's exception, whatever the signal.
    Refuse(request, "NetworkError", "Error retrieving a token.",
           /*abort_reason_first=*/false);
    return;
  }
  // The browser then fetches each provider's well-known file, its config and
  // its accounts, which the Runtime does not do yet: the request waits, until
  // its signal aborts it.
  request->set_waiting(true);
}

void CredentialsContainer::GetFromPasswordStore(
    v8::Isolate* isolate,
    v8::Local<v8::Promise::Resolver> resolver) {
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  if (password_request_pending_) {
    // CredentialManagerImpl::Get() answers PENDING_REQUEST at once.
    PostAnswer(isolate, context,
               base::BindOnce(
                   [](v8::Global<v8::Promise::Resolver> resolver,
                      v8::Isolate* isolate, v8::Local<v8::Context> context) {
                     std::ignore = resolver.Get(isolate)->Reject(
                         context, NewDOMException(isolate, "InvalidStateError",
                                                  "A request is already "
                                                  "pending."));
                   },
                   v8::Global<v8::Promise::Resolver>(isolate, resolver)));
    return;
  }
  // CredentialManagerPendingRequestTask reads the profile's password store,
  // which is empty, on a sequence of its own, and the browser then answers
  // with no credential, which resolves with null.
  password_request_pending_ = true;
  PostAnswer(
      isolate, context,
      base::BindOnce(
          [](cppgc::WeakPersistent<CredentialsContainer> container,
             v8::Global<v8::Promise::Resolver> resolver, v8::Isolate* isolate,
             v8::Local<v8::Context> context) {
            PostAnswer(
                isolate, context,
                base::BindOnce(
                    [](cppgc::WeakPersistent<CredentialsContainer> container,
                       v8::Global<v8::Promise::Resolver> resolver,
                       v8::Isolate* isolate, v8::Local<v8::Context> context) {
                      if (container) {
                        container->password_request_pending_ = false;
                      }
                      std::ignore = resolver.Get(isolate)->Resolve(
                          context, v8::Null(isolate));
                    },
                    std::move(container), std::move(resolver)));
          },
          cppgc::WeakPersistent<CredentialsContainer>(this),
          v8::Global<v8::Promise::Resolver>(isolate, resolver)));
}

CredentialsContainer::Request* CredentialsContainer::Wait(
    v8::Isolate* isolate,
    v8::Local<v8::Promise::Resolver> resolver,
    AbortSignal* signal,
    RequestKind kind) {
  Request* const request =
      bindings::MakeGarbageCollected<Request>(isolate, resolver, signal, kind);
  if (signal) {
    AbortRequest* const algorithm =
        bindings::MakeGarbageCollected<AbortRequest>(this, request);
    request->set_algorithm(algorithm);
    signal->AddAlgorithm(algorithm);
  }
  return request;
}

void CredentialsContainer::Refuse(Request* request,
                                  std::string name,
                                  std::string message,
                                  bool abort_reason_first) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::Context> context;
  if (!request->resolver(isolate)->GetCreationContext(isolate).ToLocal(
          &context)) {
    return;
  }
  PostAnswer(isolate, context,
             base::BindOnce(
                 [](cppgc::Persistent<Request> request, std::string name,
                    std::string message, bool abort_reason_first,
                    v8::Isolate* isolate, v8::Local<v8::Context> context) {
                   request->set_answered();
                   // A request whose signal aborted before the answer came
                   // rejects with the signal's reason.
                   AbortSignal* const signal = request->signal();
                   const v8::Local<v8::Value> reason =
                       abort_reason_first && signal && signal->Aborted()
                           ? signal->Reason()
                           : NewDOMException(isolate, name, message);
                   std::ignore =
                       request->resolver(isolate)->Reject(context, reason);
                 },
                 cppgc::Persistent<Request>(request), std::move(name),
                 std::move(message), abort_reason_first));
}

void CredentialsContainer::Cancel(cppgc::Member<Request>& held,
                                  std::string name,
                                  std::string message) {
  Request* const request = held.Get();
  if (!request) {
    return;
  }
  // The browser drops it before it takes any request that the page makes
  // after the abort.
  held.Clear();
  Refuse(request, std::move(name), std::move(message));
}

void CredentialsContainer::Abort(Request* request) {
  if (request->answered()) {
    return;
  }
  switch (request->kind()) {
    case RequestKind::kPublicKey:
      // AuthenticatorCommonImpl::Cancel() drops the request that the browser
      // holds, whichever request's signal aborted.
      Cancel(public_key_request_, "AbortError", "Request has been aborted.");
      return;
    case RequestKind::kOtp:
      // WebOTPService::Abort() completes the request that the service holds.
      Cancel(otp_request_, "AbortError", "OTP retrieval was aborted.");
      return;
    case RequestKind::kIdentity:
      // Each FedCM request has a pipe of its own, which its abort goes
      // through.
      if (request->waiting()) {
        request->set_waiting(false);
        Refuse(request, "AbortError", "The request has been aborted.");
      }
      return;
  }
}

}  // namespace mitto
