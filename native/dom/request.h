// The Fetch Standard's Request, with what fetch() sends: its URL, method,
// header lines and a body, and the signal that aborts it, which follows the
// signal it is given. Its other members, and RequestInit's members that
// change how a request goes, such as `cache` or `redirect`, are Known Gaps:
// a value other than the default throws.
//
// Upstream: third_party/blink/renderer/core/fetch/request.h
// Upstream: third_party/blink/renderer/core/fetch/request.cc

#ifndef MITTO_DOM_REQUEST_H_
#define MITTO_DOM_REQUEST_H_

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "url/gurl.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class AbortSignal;
class Headers;

class Request final : public bindings::ScriptWrappable {
 public:
  // The request's mode, of those that the Runtime has.
  enum class Mode {
    kCors,
    kSameOrigin,
  };

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

  // The members of the interface.
  std::string Method() const { return method_; }
  std::u16string Url() const;
  mitto::Headers* GetHeaders() const { return headers_.Get(); }
  AbortSignal* Signal() const { return signal_.Get(); }

  // What fetch() uses.
  const GURL& url() const { return url_; }
  Mode mode() const { return mode_; }
  const std::optional<std::string>& body() const { return body_; }

 private:
  GURL url_;
  std::string method_ = "GET";
  cppgc::Member<mitto::Headers> headers_;
  std::optional<std::string> body_;
  Mode mode_ = Mode::kCors;
  cppgc::Member<AbortSignal> signal_;
};

// Extracts a body from `value`, as the Fetch Standard does for a BodyInit:
// an ArrayBuffer or a view of one gives its bytes, and anything else, which
// is a string as far as the Runtime's interfaces go, gives the UTF-8 of
// String(value) and a Content-Type of text/plain;charset=UTF-8, which
// `content_type` gets. Returns nullopt if String() threw.
std::optional<std::string> ExtractBody(
    v8::Isolate* isolate,
    v8::Local<v8::Value> value,
    std::optional<std::string>* content_type);

}  // namespace mitto

#endif  // MITTO_DOM_REQUEST_H_
