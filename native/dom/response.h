// The Fetch Standard's Response, whose body is a byte stream, as Blink's:
// the bytes of a fetch's response as the Session hands them over, or of a body
// that the Cache API stored, or what a script gave its constructor, a stream
// among them. text(), json() and arrayBuffer() read all of it (BodyMixin).
// Its request's signal errors a body that has not all been read, as the abort
// of Blink's BodyStreamBuffer does. clone(), blob(), formData() and bytes()
// are Known Gaps.
//
// Upstream: third_party/blink/renderer/core/fetch/response.h
// Upstream: third_party/blink/renderer/core/fetch/response.cc

#ifndef MITTO_DOM_RESPONSE_H_
#define MITTO_DOM_RESPONSE_H_

#include <stdint.h>

#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/body_mixin.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"

namespace mitto {

class BodyStreamBuffer;
class Headers;
class ReadableStream;

class Response final : public bindings::ScriptWrappable, public BodyMixin {
 public:
  // The response's type, as Response.type gives it.
  enum class ResponseType {
    kBasic,
    kCors,
    kDefault,
    // A fetch's of mode no-cors from another origin: status 0, no URL, no
    // header lines and a null body, as the Fetch Standard's opaque filtered
    // response.
    kOpaque,
    // Response.error()'s, a network error: status 0, no URL, no header lines
    // and a null body.
    kError,
  };

  // `new Response(body, init)`.
  static Response* Create(const std::optional<v8::Local<v8::Value>>& body,
                          const bindings::ResponseInit& init,
                          bindings::ExceptionState& exception_state);
  // `Response.json(data, init)`.
  static Response* StaticJson(v8::Local<v8::Value> data,
                              const bindings::ResponseInit& init,
                              bindings::ExceptionState& exception_state);
  // `Response.error()`.
  static Response* Error();
  // `Response.redirect(url, status)`.
  static Response* Redirect(const std::u16string& url,
                            uint16_t status,
                            bindings::ExceptionState& exception_state);
  // One that fetch() got, or that the Cache API stored, whose body is `body`,
  // or none for a null body. An opaque one's body is null, and `body` is
  // that of its internal response, which no script reads, as Blink's opaque
  // filtered response holds it.
  static Response* CreateFetched(ResponseType type,
                                 int32_t status,
                                 std::string status_text,
                                 std::u16string url,
                                 bool redirected,
                                 Headers* headers,
                                 BodyStreamBuffer* body);

  Response();
  ~Response() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface; BodyMixin has the others.
  std::u16string Type() const;
  std::u16string Url() const { return url_; }
  bool Redirected() const { return redirected_; }
  uint16_t Status() const { return static_cast<uint16_t>(status_); }
  bool Ok() const { return status_ >= 200 && status_ <= 299; }
  std::string StatusText() const { return status_text_; }
  mitto::Headers* Headers() const { return headers_.Get(); }
  ReadableStream* Body() const { return BodyStream(); }
  // A copy with a tee of its body, as Blink's clone() makes it.
  Response* Clone(bindings::ExceptionState& exception_state);

  // What the Runtime reads of it itself, as Blink's FetchDataLoader reads a
  // body, such as Cache.put() does.
  //
  // Whether its body is null, as an opaque response's is.
  bool HasNullBody() const { return !body_buffer_; }
  // Uses the body, as a read by a script does, and calls `done` with all of it
  // once it has come, or with nullopt if it failed, and whether its signal
  // aborted it. A null body reads as empty. Its body must be neither locked
  // nor used.
  void ReadAll(base::OnceCallback<void(std::optional<std::string> body,
                                       bool aborted)> done);

 private:
  // BodyMixin:
  BodyStreamBuffer* BodyBuffer() const override;
  std::string ContentType() const override;

  ResponseType type_ = ResponseType::kDefault;
  int32_t status_ = 200;
  std::string status_text_;
  std::u16string url_;
  bool redirected_ = false;
  cppgc::Member<mitto::Headers> headers_;
  cppgc::Member<BodyStreamBuffer> body_buffer_;
  // An opaque response's internal body, which goes on loading.
  cppgc::Member<BodyStreamBuffer> internal_body_buffer_;
};

}  // namespace mitto

#endif  // MITTO_DOM_RESPONSE_H_
