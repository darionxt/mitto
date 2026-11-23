#include "mitto/dom/body_mixin.h"

#include <string>
#include <utility>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/notreached.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/runtime_features.h"
#include "mitto/bindings/script_promise.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/dom/blob.h"
#include "mitto/dom/body_stream_buffer.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/fetch_data_loader.h"
#include "mitto/dom/readable_stream.h"
#include "mitto/dom/stream_chunks.h"
#include "mitto/dom/text_decoder_transformer.h"
#include "mitto/dom/transform_stream.h"
#include "mitto/dom/window_task.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-json.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

// Blink's BodyConsumerBase with its subclasses for arrayBuffer(), json() and
// text(): what a read of the whole body settles once its loader has all of
// it.
class BodyConsumer final : public cppgc::GarbageCollected<BodyConsumer>,
                           public FetchDataLoader::Client {
 public:
  enum class Kind {
    kArrayBuffer,
    kBlob,
    kBytes,
    kJson,
    kText,
  };

  // `mime_type` is the type of a Blob that blob() makes.
  BodyConsumer(Kind kind,
               bindings::ScriptPromiseResolver* resolver,
               ExecutionContext* context,
               std::string mime_type = std::string())
      : kind_(kind),
        resolver_(resolver),
        context_(context),
        mime_type_(std::move(mime_type)) {}

  void DidFetchDataLoadedArrayBuffer(std::string bytes) override {
    switch (kind_) {
      case Kind::kArrayBuffer:
        ResolveLater(base::BindOnce(
            [](std::string bytes,
               v8::Isolate* isolate) -> v8::Local<v8::Value> {
              return NewArrayBuffer(isolate, base::as_byte_span(bytes));
            },
            std::move(bytes)));
        return;
      case Kind::kBytes:
        ResolveLater(base::BindOnce(
            [](std::string bytes,
               v8::Isolate* isolate) -> v8::Local<v8::Value> {
              return NewUint8Array(isolate, base::as_byte_span(bytes));
            },
            std::move(bytes)));
        return;
      case Kind::kBlob:
        // Blink's loader makes a blob of the body's bytes, of the body's MIME
        // type, as FetchDataLoaderAsBlobHandle does.
        ResolveLater(base::BindOnce(
            [](std::string bytes, std::string mime_type,
               v8::Isolate* isolate) -> v8::Local<v8::Value> {
              return bindings::ToV8(
                  isolate,
                  mitto::Blob::Create(base::as_byte_span(bytes), mime_type));
            },
            std::move(bytes), mime_type_));
        return;
      case Kind::kJson:
      case Kind::kText:
        break;
    }
    NOTREACHED();
  }

  void DidFetchDataLoadedString(std::u16string string) override {
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    if (kind_ == Kind::kText) {
      ResolveLater(base::BindOnce(
          [](std::u16string string, v8::Isolate* isolate) {
            return v8::Local<v8::Value>(bindings::ToV8String(isolate, string));
          },
          std::move(string)));
      return;
    }
    CHECK_EQ(kind_, Kind::kJson);
    const v8::TryCatch try_catch(isolate);
    v8::Local<v8::Value> parsed;
    if (!v8::JSON::Parse(isolate->GetCurrentContext(),
                         bindings::ToV8String(isolate, string))
             .ToLocal(&parsed)) {
      if (try_catch.HasCaught()) {
        resolver_->Reject(isolate, try_catch.Exception());
      }
      return;
    }
    ResolveLater(
        base::BindOnce([](v8::Global<v8::Value> parsed,
                          v8::Isolate* isolate) { return parsed.Get(isolate); },
                       v8::Global<v8::Value>(isolate, parsed)));
  }

  void DidFetchDataLoadFailed() override {
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    resolver_->Reject(isolate,
                      v8::Exception::TypeError(v8::String::NewFromUtf8Literal(
                          isolate, "Failed to fetch")));
  }

  void Abort(v8::Local<v8::Value> reason) override {
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    if (bindings::PerIsolateData::From(isolate)->features().IsEnabled(
            bindings::RuntimeFeature::kForwardReasonToFetchBodyAbort)) {
      // This is the standardized behavior, but is currently behind an
      // experiment as it's a potentially breaking change.
      resolver_->Reject(isolate, reason);
    } else {
      resolver_->Reject(
          isolate,
          bindings::PerIsolateData::From(isolate)->dom_exception_hooks().create(
              isolate, "AbortError", "The user aborted a request."));
    }
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(resolver_);
    visitor->Trace(context_);
    FetchDataLoader::Client::Trace(visitor);
  }

 private:
  using MakeValue =
      base::OnceCallback<v8::Local<v8::Value>(v8::Isolate* isolate)>;

  // Resource Timing's entry is not yet added, so the resolution waits for a
  // later networking task, as Blink's does (https://crbug.com/507169).
  void ResolveLater(MakeValue make_value) {
    PostTaskInContext(
        context_.Get(), TaskType::kNetworking,
        base::BindOnce(
            [](cppgc::Persistent<BodyConsumer> self, MakeValue make_value) {
              v8::Isolate* const isolate = v8::Isolate::GetCurrent();
              self->resolver_->Resolve(isolate,
                                       std::move(make_value).Run(isolate));
            },
            cppgc::Persistent<BodyConsumer>(this), std::move(make_value)));
  }

  const Kind kind_;
  const cppgc::Member<bindings::ScriptPromiseResolver> resolver_;
  const cppgc::WeakMember<ExecutionContext> context_;
  const std::string mime_type_;
};

}  // namespace

v8::Local<v8::Promise> BodyMixin::ArrayBuffer(
    bindings::ExceptionState& exception_state) {
  return LoadAndConvertBody(ReadAs::kArrayBuffer, exception_state);
}

v8::Local<v8::Promise> BodyMixin::Blob(
    bindings::ExceptionState& exception_state) {
  return LoadAndConvertBody(ReadAs::kBlob, exception_state);
}

v8::Local<v8::Promise> BodyMixin::Bytes(
    bindings::ExceptionState& exception_state) {
  return LoadAndConvertBody(ReadAs::kBytes, exception_state);
}

v8::Local<v8::Promise> BodyMixin::Json(
    bindings::ExceptionState& exception_state) {
  return LoadAndConvertBody(ReadAs::kJson, exception_state);
}

v8::Local<v8::Promise> BodyMixin::Text(
    bindings::ExceptionState& exception_state) {
  return LoadAndConvertBody(ReadAs::kText, exception_state);
}

v8::Local<v8::Promise> BodyMixin::LoadAndConvertBody(
    ReadAs read_as,
    bindings::ExceptionState& exception_state) {
  RejectInvalidConsumption(exception_state);
  if (exception_state.HadException()) {
    return v8::Local<v8::Promise>();
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  auto* const resolver =
      bindings::MakeGarbageCollected<bindings::ScriptPromiseResolver>(isolate);
  const v8::Local<v8::Promise> promise = resolver->Promise(isolate);
  if (BodyStreamBuffer* const body_buffer = BodyBuffer()) {
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();
    ExecutionContext* const execution_context =
        ExecutionContext::From(isolate, context);
    BodyConsumer::Kind kind = BodyConsumer::Kind::kText;
    FetchDataLoader* loader = nullptr;
    switch (read_as) {
      case ReadAs::kArrayBuffer:
        kind = BodyConsumer::Kind::kArrayBuffer;
        loader = FetchDataLoader::CreateLoaderAsArrayBuffer();
        break;
      case ReadAs::kBlob:
        kind = BodyConsumer::Kind::kBlob;
        loader = FetchDataLoader::CreateLoaderAsArrayBuffer();
        break;
      case ReadAs::kBytes:
        kind = BodyConsumer::Kind::kBytes;
        loader = FetchDataLoader::CreateLoaderAsArrayBuffer();
        break;
      case ReadAs::kJson:
        kind = BodyConsumer::Kind::kJson;
        loader = FetchDataLoader::CreateLoaderAsString();
        break;
      case ReadAs::kText:
        kind = BodyConsumer::Kind::kText;
        loader = FetchDataLoader::CreateLoaderAsString();
        break;
    }
    body_buffer->StartLoading(
        loader,
        bindings::MakeGarbageCollected<BodyConsumer>(
            kind, resolver, execution_context,
            read_as == ReadAs::kBlob ? MimeType() : std::string()),
        exception_state);
    if (exception_state.HadException()) {
      return v8::Local<v8::Promise>();
    }
    return promise;
  }
  // A null body.
  switch (read_as) {
    case ReadAs::kArrayBuffer:
      resolver->Resolve(isolate, v8::ArrayBuffer::New(isolate, 0));
      break;
    case ReadAs::kBlob:
      resolver->Resolve(
          isolate, bindings::ToV8(
                       isolate, mitto::Blob::Create(base::span<const uint8_t>(),
                                                    MimeType())));
      break;
    case ReadAs::kBytes:
      resolver->Resolve(isolate,
                        NewUint8Array(isolate, base::span<const uint8_t>()));
      break;
    case ReadAs::kJson:
      resolver->Reject(
          isolate, v8::Exception::SyntaxError(v8::String::NewFromUtf8Literal(
                       isolate, "Unexpected end of input")));
      break;
    case ReadAs::kText:
      resolver->Resolve(isolate, v8::String::Empty(isolate));
      break;
  }
  return promise;
}

ReadableStream* BodyMixin::BodyStream() const {
  if (BodyStreamBuffer* const body_buffer = BodyBuffer()) {
    return body_buffer->Stream();
  }
  return nullptr;
}

ReadableStream* BodyMixin::TextStream(
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  ReadableStream* const body_stream = BodyStream();
  if (!body_stream) {
    // An empty stream, closed.
    ReadableStream* const stream =
        ReadableStream::Create(std::nullopt, std::nullopt, exception_state);
    if (stream && !exception_state.HadException()) {
      stream->CloseStream(isolate, exception_state);
    }
    return stream;
  }
  RejectInvalidConsumption(exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  // Always UTF-8, whatever the header lines say
  // (https://github.com/whatwg/fetch/pull/1862).
  auto* const transformer =
      bindings::MakeGarbageCollected<TextDecoderTransformer>(
          "UTF-8", /*fatal=*/false, /*ignore_bom=*/false);
  TransformStream* const transform_stream =
      TransformStream::Create(isolate, transformer, exception_state);
  if (exception_state.HadException() || !transform_stream) {
    return nullptr;
  }
  bindings::ReadableWritablePair pair;
  pair.readable = transform_stream->Readable();
  pair.writable = transform_stream->Writable();
  ReadableStream* const piped_stream =
      body_stream->PipeThrough(pair, std::nullopt, exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  return piped_stream;
}

bool BodyMixin::IsBodyUsed() const {
  BodyStreamBuffer* const body_buffer = BodyBuffer();
  return body_buffer && body_buffer->IsStreamDisturbed();
}

bool BodyMixin::IsBodyLocked() const {
  BodyStreamBuffer* const body_buffer = BodyBuffer();
  return body_buffer && body_buffer->IsStreamLocked();
}

void BodyMixin::RejectInvalidConsumption(
    bindings::ExceptionState& exception_state) const {
  // Blink throws both for a body that is locked and used, and the second
  // stands.
  if (IsBodyUsed()) {
    exception_state.ThrowTypeError("body stream already read");
  } else if (IsBodyLocked()) {
    exception_state.ThrowTypeError("body stream is locked");
  }
}

}  // namespace mitto
