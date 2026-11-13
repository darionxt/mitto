// The Fetch Standard's Response, whose body arrives as the Session reads it:
// text(), json() and arrayBuffer() wait for all of it, and give the Session
// credit for more only once one of them reads, so that a body that no script
// reads slows the server down, as a full Mojo data pipe does in Chrome. Its
// request's signal errors a body that has not all been read, as the abort of
// Blink's BodyStreamBuffer does. Its stream, clone(), blob(), formData() and
// bytes() are Known Gaps.
//
// Upstream: third_party/blink/renderer/core/fetch/response.h
// Upstream: third_party/blink/renderer/core/fetch/response.cc
// Upstream: third_party/blink/renderer/core/fetch/body.cc
// Upstream: third_party/blink/renderer/core/fetch/body_stream_buffer.cc

#ifndef MITTO_DOM_RESPONSE_H_
#define MITTO_DOM_RESPONSE_H_

#include <stdint.h>

#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/prefinalizer.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-traced-handle.h"

namespace mitto {

class AbortSignal;
class FetchLoader;
class Headers;

class Response final : public bindings::ScriptWrappable {
  CPPGC_USING_PRE_FINALIZER(Response, Dispose);

 public:
  // The response's type, as Response.type gives it.
  enum class ResponseType {
    kBasic,
    kCors,
    kDefault,
  };

  // `new Response(body, init)`.
  static Response* Create(const std::optional<v8::Local<v8::Value>>& body,
                          const bindings::ResponseInit& init,
                          bindings::ExceptionState& exception_state);
  // `Response.json(data, init)`.
  static Response* StaticJson(v8::Local<v8::Value> data,
                              const bindings::ResponseInit& init,
                              bindings::ExceptionState& exception_state);
  // One that fetch() got, whose body `loader` brings, or none for a null body.
  static Response* CreateFetched(ResponseType type,
                                 int32_t status,
                                 std::string status_text,
                                 std::u16string url,
                                 bool redirected,
                                 Headers* headers,
                                 FetchLoader* loader);

  Response();
  ~Response() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  std::u16string Type() const;
  std::u16string Url() const { return url_; }
  bool Redirected() const { return redirected_; }
  uint16_t Status() const { return static_cast<uint16_t>(status_); }
  bool Ok() const { return status_ >= 200 && status_ <= 299; }
  std::string StatusText() const { return status_text_; }
  mitto::Headers* Headers() const { return headers_.Get(); }
  bool BodyUsed() const { return body_used_; }
  v8::Local<v8::Promise> ArrayBuffer(bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Json(bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Text(bindings::ExceptionState& exception_state);

  // What its FetchLoader tells it.
  //
  // The signal of the request, whose abort errors a body that is not all read
  // yet with its reason.
  void FollowSignal(AbortSignal* signal);
  void AppendBody(std::string_view bytes);
  // The body ended: 0 when all of it came, and otherwise a net error.
  void EndBody(int net_error);
  // Whether a script waits for the body, so that what arrives is taken.
  bool HasReader() const {
    return !reader_.IsEmpty() || !whole_body_reader_.is_null();
  }

  // What the Runtime reads of it itself, as Blink's FetchDataLoader reads a
  // body, such as WebAssembly.compileStreaming() does.
  //
  // Whether its body is null, as a 204's is.
  bool HasNullBody() const { return body_state_ == BodyState::kNull; }
  // Uses the body, as a read by a script does, and calls `done` with all of it
  // once it has come, or with nullopt if it failed. A null body reads as
  // empty. Its body must not be used.
  void ReadAll(base::OnceCallback<void(std::optional<std::string> body)> done);

 private:
  enum class BodyState {
    kNull,
    kLoading,
    kComplete,
    kFailed,
    // The request's signal aborted before all of the body was read.
    kAborted,
  };
  enum class ReadAs {
    kArrayBuffer,
    kJson,
    kText,
  };

  // Starts reading the body for a script, and returns its promise.
  v8::Local<v8::Promise> Read(ReadAs read_as,
                              bindings::ExceptionState& exception_state);
  // Settles `resolver` with the whole body, as `read_as` reads it.
  void Settle(v8::Local<v8::Promise::Resolver> resolver, ReadAs read_as);
  // The whole body, which this gives up, or nullopt if it failed.
  std::optional<std::string> TakeBody();
  void Dispose();
  // The request's signal aborted.
  void AbortBody();

  ResponseType type_ = ResponseType::kDefault;
  int32_t status_ = 200;
  std::string status_text_;
  std::u16string url_;
  bool redirected_ = false;
  cppgc::Member<mitto::Headers> headers_;
  BodyState body_state_ = BodyState::kNull;
  std::string body_;
  bool body_used_ = false;
  // Whether a script read a null body, which Chrome's empty stream would have
  // used up, so that an abort leaves it alone.
  bool null_body_read_ = false;
  // The promise of the read that waits for the rest of the body.
  v8::TracedReference<v8::Promise::Resolver> reader_;
  ReadAs reader_read_as_ = ReadAs::kText;
  // What ReadAll() calls back once the rest of the body has come.
  base::OnceCallback<void(std::optional<std::string>)> whole_body_reader_;
  // What brings the body while it loads.
  raw_ptr<FetchLoader> loader_ = nullptr;
  cppgc::Member<AbortSignal> signal_;
  // What a read of an aborted body rejects with.
  v8::TracedReference<v8::Value> abort_reason_;

  friend class FetchLoader;
  friend class AbortBodyAlgorithm;
};

}  // namespace mitto

#endif  // MITTO_DOM_RESPONSE_H_
