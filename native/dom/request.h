// The Fetch Standard's Request, with what fetch() sends: its URL, method,
// header lines and a body, its mode, credentials mode, referrer, referrer
// policy, priority and keepalive flag, and the signal that aborts it, which
// follows the signal it is given. Its body is a byte stream, as a Response's
// is (BodyMixin), which a script reads as it reads a Response's. Its other
// members, and RequestInit's members that change how a request goes, such as
// `cache` or `redirect`, are Known Gaps: a value other than the default
// throws.
//
// Upstream: third_party/blink/renderer/core/fetch/request.h
// Upstream: third_party/blink/renderer/core/fetch/request.cc

#ifndef MITTO_DOM_REQUEST_H_
#define MITTO_DOM_REQUEST_H_

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/blob_data.h"
#include "mitto/dom/body_mixin.h"
#include "mitto/dom/fetcher.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class AbortSignal;
class BodyStreamBuffer;
class Headers;
class ReadableStream;

class Request final : public bindings::ScriptWrappable, public BodyMixin {
 public:
  // `new Request(input, init)`, and the first step of fetch().
  static Request* Create(const bindings::RequestOrUSVString& input,
                         const bindings::RequestInit& init,
                         bindings::ExceptionState& exception_state);
  // One that the Cache API stored, as Blink makes a Request of the browser's
  // FetchAPIRequest: its URL, method and header lines, `headers` each
  // "name: value", and a signal of its own.
  static Request* CreateFromStored(
      GURL url,
      std::string method,
      const std::vector<std::pair<std::string, std::string>>& headers);

  Request();
  ~Request() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface; BodyMixin has the others.
  std::string Method() const { return method_; }
  std::u16string Url() const;
  mitto::Headers* GetHeaders() const { return headers_.Get(); }
  AbortSignal* Signal() const { return signal_.Get(); }
  std::u16string Duplex() const { return u"half"; }
  ReadableStream* Body() const { return BodyStream(); }
  // A Request that a script makes has no destination, as Blink's has none.
  std::u16string Destination() const { return std::u16string(); }
  // "about:client", the empty string for no referrer, or the URL.
  std::u16string Referrer() const;
  std::u16string GetReferrerPolicy() const;
  std::u16string Mode() const;
  std::u16string Credentials() const;
  std::u16string Cache() const;
  std::u16string Redirect() const;
  std::u16string Integrity() const;
  bool Keepalive() const { return keepalive_; }
  // Its target address space, which only the default can set yet.
  std::u16string TargetAddressSpace() const { return u"unknown"; }
  bool IsHistoryNavigation() const { return false; }
  bool IsReloadNavigation() const { return false; }
  // A copy with a tee of its body and a signal that follows its own, as
  // Blink's clone() makes it.
  Request* Clone(bindings::ExceptionState& exception_state);

  // What fetch() uses.
  const GURL& url() const { return url_; }
  FetchMode mode() const { return mode_; }
  FetchCredentialsMode credentials() const { return credentials_; }
  // kClientReferrer, kNoReferrer, or a URL of the document's origin.
  const std::string& referrer() const { return referrer_; }
  FetchReferrerPolicy referrer_policy() const { return referrer_policy_; }
  FetchPriorityHint priority() const { return priority_; }
  bool keepalive() const { return keepalive_; }
  FetchCacheMode cache_mode() const { return cache_mode_; }
  FetchRedirectMode redirect_mode() const { return redirect_mode_; }
  const std::u16string& integrity() const { return integrity_; }
  // For a blob: URL, the blob that it named when the Request was made, as
  // Blink resolves it then, so that a fetch works after the URL is revoked,
  // and the origin of the context that made it; null and nullopt when it
  // named none.
  const scoped_refptr<BlobDataHandle>& blob_url_handle() const {
    return blob_url_handle_;
  }
  const std::optional<url::Origin>& blob_url_origin() const {
    return blob_url_origin_;
  }
  // The length of its body as Blink's FetchRequestData::BufferByteLength()
  // holds it, which fetchLater() counts: its init's body's, or its input's.
  uint64_t body_length() const { return body_length_; }
  // Takes its body for fetch(), as Blink's FetchRequestData::Pass() does,
  // leaving it a body that is closed, locked and disturbed; null for a null
  // body.
  BodyStreamBuffer* PassBody(bindings::ExceptionState& exception_state);

 private:
  // BodyMixin:
  BodyStreamBuffer* BodyBuffer() const override;
  std::string ContentType() const override;

  GURL url_;
  std::string method_ = "GET";
  cppgc::Member<mitto::Headers> headers_;
  cppgc::Member<BodyStreamBuffer> body_buffer_;
  uint64_t body_length_ = 0;
  FetchMode mode_ = FetchMode::kCors;
  FetchCredentialsMode credentials_ = FetchCredentialsMode::kSameOrigin;
  std::string referrer_ = kClientReferrer;
  FetchReferrerPolicy referrer_policy_ = FetchReferrerPolicy::kDefault;
  FetchPriorityHint priority_ = FetchPriorityHint::kAuto;
  bool keepalive_ = false;
  FetchCacheMode cache_mode_ = FetchCacheMode::kDefault;
  FetchRedirectMode redirect_mode_ = FetchRedirectMode::kFollow;
  // Its integrity metadata.
  std::u16string integrity_;
  scoped_refptr<BlobDataHandle> blob_url_handle_;
  std::optional<url::Origin> blob_url_origin_;
  cppgc::Member<AbortSignal> signal_;
};

// Extracts a body from `value`, as the Fetch Standard does for a BodyInit
// other than a ReadableStream: a Blob gives its bytes, read as Blink's
// BlobBytesConsumer reads them, and its type, if it has one, as the
// Content-Type that `content_type` gets; an ArrayBuffer or a view of one gives
// its bytes; a FormData gives a multipart/form-data body, read as a blob if it
// holds a file, and a URLSearchParams an application/x-www-form-urlencoded
// one; and anything else, which is a string as far as the Runtime's
// interfaces go, gives the UTF-8 of String(value) and a Content-Type of
// text/plain;charset=UTF-8. Returns null if
// String() threw, or after throwing for a buffer larger than Blink takes.
// `length`, if given, gets the body's length as Blink's ExtractBody() records
// it: a string's in UTF-16 code units, and every other body's in bytes.
BodyStreamBuffer* ExtractBody(v8::Isolate* isolate,
                              v8::Local<v8::Value> value,
                              std::optional<std::string>* content_type,
                              bindings::ExceptionState& exception_state,
                              uint64_t* length = nullptr);

}  // namespace mitto

#endif  // MITTO_DOM_REQUEST_H_
