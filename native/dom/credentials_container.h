// The Credential Management API's CredentialsContainer, navigator.credentials,
// whose get() gives what Chrome gives a top-level document of a new profile:
// no saved password or passkey, no phone that forwards texts, and no user
// activation. Blink's checks of each kind of request come first, then what the
// browser answers:
//
// - passwords and federated credentials: null, once the browser has read its
//   empty password store, which takes one request at a time;
// - WebAuthn: Chrome's dialog, which waits for a person who never answers, and
//   which, if it times out, waits for them to dismiss that, in the browser's
//   one slot, until an abort cancels it; a document on an IP address is
//   refused at once;
// - WebOTP: a wait for a text, which no phone forwards, until the browser
//   gives up after four minutes;
// - digital credentials: a refusal, for want of a user's activation;
// - FedCM: a wait, since the browser asks the identity provider's servers for
//   its files, which the Runtime does not fetch yet.
//
// Its other members are Known Gaps.
//
// Upstream: third_party/blink/renderer/modules/credentialmanagement/authentication_credentials_container.cc
// Upstream: third_party/blink/renderer/modules/credentialmanagement/digital_identity_credential.cc
// Upstream: third_party/blink/renderer/modules/credentialmanagement/credential_utils.cc
// Upstream: components/password_manager/core/browser/credential_manager_impl.cc
// Upstream: content/browser/webauth/authenticator_common_impl.cc
// Upstream: components/webauthn/core/browser/webauthn_security_utils.cc
// Upstream: content/browser/webauth/webauth_request_security_checker_impl.cc
// Upstream: chrome/browser/webauthn/chrome_authenticator_request_delegate.cc
// Upstream: content/browser/sms/webotp_service.cc
// Upstream: chrome/browser/sharing/sms/sms_remote_fetcher_ui_controller.cc
// Upstream: content/browser/webid/request_handler.cc

#ifndef MITTO_DOM_CREDENTIALS_CONTAINER_H_
#define MITTO_DOM_CREDENTIALS_CONTAINER_H_

#include <string>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-promise.h"

namespace mitto::bindings {
class ExceptionState;
struct CredentialRequestOptions;
struct DigitalCredentialRequestOptions;
}  // namespace mitto::bindings

namespace mitto {

class AbortSignal;

class CredentialsContainer final : public bindings::ScriptWrappable {
 public:
  static CredentialsContainer* Create();

  CredentialsContainer();
  ~CredentialsContainer() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  v8::Local<v8::Promise> Get(const bindings::CredentialRequestOptions& options,
                             bindings::ExceptionState& exception_state);

 private:
  class Request;
  class AbortRequest;

  // Which of the browser's services a request waits on.
  enum class RequestKind { kPublicKey, kOtp, kIdentity };

  // DiscoverDigitalIdentityCredentialFromExternalSource().
  void GetDigital(v8::Isolate* isolate,
                  v8::Local<v8::Promise::Resolver> resolver,
                  const bindings::DigitalCredentialRequestOptions& digital);
  // ForwardRequestToAuthenticator() and what the browser's authenticator
  // answers.
  void GetPublicKey(v8::Isolate* isolate,
                    v8::Local<v8::Promise::Resolver> resolver,
                    const bindings::CredentialRequestOptions& options);
  // The WebOTP service's Receive().
  void GetOtp(v8::Isolate* isolate,
              v8::Local<v8::Promise::Resolver> resolver,
              AbortSignal* signal);
  // GetForIdentity() and what the browser refuses before it asks the identity
  // providers.
  void GetIdentity(v8::Isolate* isolate,
                   v8::Local<v8::Promise::Resolver> resolver,
                   const bindings::CredentialRequestOptions& options);
  // CredentialManagerImpl::Get(): null once the password store is read.
  void GetFromPasswordStore(v8::Isolate* isolate,
                            v8::Local<v8::Promise::Resolver> resolver);
  // A request for the browser, which its signal may abort.
  Request* Wait(v8::Isolate* isolate,
                v8::Local<v8::Promise::Resolver> resolver,
                AbortSignal* signal,
                RequestKind kind);
  // The browser's answer to `request`, from a task of its own: a DOMException
  // of `name` and `message`, or with `abort_reason_first`, the reason of the
  // request's signal if it has aborted by then, as OnGetAssertionComplete(),
  // OnSmsReceive() and OnRequestToken() reject.
  void Refuse(Request* request,
              std::string name,
              std::string message,
              bool abort_reason_first = true);
  // The browser drops the request that `held` holds, if any, which rejects as
  // Refuse() does.
  void Cancel(cppgc::Member<Request>& held,
              std::string name,
              std::string message);
  // The signal of a request that the browser has not answered aborted: Blink
  // tells the browser, which drops what that tells it to.
  void Abort(Request* request);
  // WebOTPService::OnTimeout(): the browser gives up on `request`.
  void OtpTimedOut(Request* request);

  // Whether the browser's CredentialManagerImpl has a request whose answer
  // waits on the password store, its pending_request_.
  bool password_request_pending_ = false;
  // The WebAuthn request that the browser holds, its req_state_.
  cppgc::Member<Request> public_key_request_;
  // The WebOTP request that the browser holds, its callback_.
  cppgc::Member<Request> otp_request_;
};

}  // namespace mitto

#endif  // MITTO_DOM_CREDENTIALS_CONTAINER_H_
