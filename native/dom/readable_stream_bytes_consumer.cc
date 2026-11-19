#include "mitto/dom/readable_stream_bytes_consumer.h"

#include "base/check.h"
#include "base/check_op.h"
#include "base/compiler_specific.h"
#include "base/functional/bind.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/miscellaneous_operations.h"
#include "mitto/dom/read_request.h"
#include "mitto/dom/readable_stream.h"
#include "mitto/dom/readable_stream_default_reader.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-persistent-handle.h"

namespace mitto {

class ReadableStreamBytesConsumer::BytesConsumerReadRequest final
    : public ReadRequest {
 public:
  explicit BytesConsumerReadRequest(ReadableStreamBytesConsumer* consumer)
      : consumer_(consumer) {}

  void ChunkSteps(v8::Isolate* isolate,
                  v8::Local<v8::Value> chunk,
                  bindings::ExceptionState& exception_state) const override {
    if (!chunk->IsUint8Array()) {
      consumer_->OnRejected();
      return;
    }
    consumer_->OnRead(chunk.As<v8::Uint8Array>());
  }

  void CloseSteps(v8::Isolate* isolate) const override {
    consumer_->OnReadDone();
  }

  void ErrorSteps(v8::Isolate* isolate, v8::Local<v8::Value> e) const override {
    consumer_->OnRejected();
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(consumer_);
    ReadRequest::Trace(visitor);
  }

 private:
  cppgc::Member<ReadableStreamBytesConsumer> consumer_;
};

ReadableStreamBytesConsumer::ReadableStreamBytesConsumer(v8::Isolate* isolate,
                                                         ReadableStream* stream)
    : isolate_(isolate) {
  DCHECK(!ReadableStream::IsLocked(stream));
  // Since the stream is not locked, AcquireDefaultReader cannot fail.
  bindings::ExceptionState exception_state(isolate);
  reader_ =
      ReadableStream::AcquireDefaultReader(isolate, stream, exception_state);
  DCHECK(!exception_state.HadException());
}

ReadableStreamBytesConsumer::~ReadableStreamBytesConsumer() = default;

BytesConsumer::Result ReadableStreamBytesConsumer::BeginRead(
    base::span<const char>& buffer) {
  buffer = {};
  if (state_ == PublicState::kErrored) {
    return Result::kError;
  }
  if (state_ == PublicState::kClosed) {
    return Result::kDone;
  }
  if (!pending_buffer_.IsEmpty()) {
    const v8::Local<v8::Uint8Array> pending = pending_buffer_.Get(isolate_);
    // The Uint8Array has become detached due to, for example, the site
    // transferring it away via postMessage(). Since we were in the middle of
    // reading the array we must error out.
    if (pending->Buffer()->WasDetached()) {
      SetErrored();
      return Result::kError;
    }
    DCHECK_LE(pending_offset_, pending->ByteLength());
    // SAFETY: the view's bytes lie at its offset in its buffer, which is not
    // detached.
    buffer =
        UNSAFE_BUFFERS(
            base::span(static_cast<const char*>(pending->Buffer()->Data()) +
                           pending->ByteOffset(),
                       pending->ByteLength()))
            .subspan(pending_offset_);
    return Result::kOk;
  }
  if (!is_reading_) {
    is_reading_ = true;
    is_inside_read_ = true;
    DCHECK(reader_);
    bindings::ExceptionState exception_state(isolate_);
    auto* const read_request =
        bindings::MakeGarbageCollected<BytesConsumerReadRequest>(this);
    ReadableStreamDefaultReader::Read(isolate_, reader_, read_request,
                                      exception_state);
    is_inside_read_ = false;
  }
  return Result::kShouldWait;
}

BytesConsumer::Result ReadableStreamBytesConsumer::EndRead(size_t read_size) {
  DCHECK(!pending_buffer_.IsEmpty());
  const v8::Local<v8::Uint8Array> pending = pending_buffer_.Get(isolate_);
  // While the buffer size is immutable once constructed, the buffer can be
  // detached if the site does something like transfer it away using
  // postMessage(). Since we were in the middle of a read we must error out.
  if (pending->Buffer()->WasDetached()) {
    SetErrored();
    return Result::kError;
  }
  DCHECK_LE(pending_offset_ + read_size, pending->ByteLength());
  pending_offset_ += read_size;
  if (pending_offset_ >= pending->ByteLength()) {
    pending_buffer_.Reset();
    pending_offset_ = 0;
  }
  return Result::kOk;
}

void ReadableStreamBytesConsumer::SetClient(Client* client) {
  DCHECK(!client_);
  DCHECK(client);
  client_ = client;
}

void ReadableStreamBytesConsumer::ClearClient() {
  client_ = nullptr;
}

void ReadableStreamBytesConsumer::Cancel() {
  if (state_ == PublicState::kClosed || state_ == PublicState::kErrored) {
    return;
  }
  {
    // We ignore exceptions as we can do nothing here.
    const v8::TryCatch try_catch(isolate_);
    bindings::ExceptionState exception_state(isolate_);
    reader_->Cancel(std::nullopt, exception_state);
  }
  state_ = PublicState::kClosed;
  ClearClient();
  reader_ = nullptr;
}

BytesConsumer::PublicState ReadableStreamBytesConsumer::GetPublicState() const {
  return state_;
}

void ReadableStreamBytesConsumer::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(reader_);
  visitor->Trace(client_);
  visitor->Trace(pending_buffer_);
  BytesConsumer::Trace(visitor);
}

void ReadableStreamBytesConsumer::OnRead(v8::Local<v8::Uint8Array> buffer) {
  DCHECK(is_reading_);
  DCHECK(pending_buffer_.IsEmpty());
  DCHECK(!pending_offset_);
  if (is_inside_read_) {
    EnqueueMicrotask(
        isolate_,
        base::BindOnce(
            [](cppgc::Persistent<ReadableStreamBytesConsumer> consumer,
               v8::Isolate* isolate, v8::Global<v8::Uint8Array> buffer) {
              const v8::HandleScope handle_scope(isolate);
              consumer->OnRead(buffer.Get(isolate));
            },
            cppgc::Persistent<ReadableStreamBytesConsumer>(this),
            isolate_.get(), v8::Global<v8::Uint8Array>(isolate_, buffer)));
    return;
  }
  is_reading_ = false;
  if (state_ == PublicState::kClosed) {
    return;
  }
  DCHECK_EQ(state_, PublicState::kReadableOrWaiting);
  pending_buffer_.Reset(isolate_, buffer);
  if (client_) {
    client_->OnStateChange();
  }
}

void ReadableStreamBytesConsumer::OnReadDone() {
  DCHECK(is_reading_);
  DCHECK(pending_buffer_.IsEmpty());
  if (is_inside_read_) {
    EnqueueMicrotask(
        isolate_,
        base::BindOnce(
            [](cppgc::Persistent<ReadableStreamBytesConsumer> consumer) {
              consumer->OnReadDone();
            },
            cppgc::Persistent<ReadableStreamBytesConsumer>(this)));
    return;
  }
  is_reading_ = false;
  if (state_ == PublicState::kClosed) {
    return;
  }
  DCHECK_EQ(state_, PublicState::kReadableOrWaiting);
  state_ = PublicState::kClosed;
  reader_ = nullptr;
  Client* const client = client_.Get();
  ClearClient();
  if (client) {
    client->OnStateChange();
  }
}

void ReadableStreamBytesConsumer::OnRejected() {
  DCHECK(is_reading_);
  DCHECK(pending_buffer_.IsEmpty());
  if (is_inside_read_) {
    EnqueueMicrotask(
        isolate_,
        base::BindOnce(
            [](cppgc::Persistent<ReadableStreamBytesConsumer> consumer) {
              consumer->OnRejected();
            },
            cppgc::Persistent<ReadableStreamBytesConsumer>(this)));
    return;
  }
  is_reading_ = false;
  if (state_ == PublicState::kClosed) {
    return;
  }
  DCHECK_EQ(state_, PublicState::kReadableOrWaiting);
  Client* const client = client_.Get();
  SetErrored();
  if (client) {
    client->OnStateChange();
  }
}

void ReadableStreamBytesConsumer::SetErrored() {
  DCHECK_NE(state_, PublicState::kClosed);
  DCHECK_NE(state_, PublicState::kErrored);
  state_ = PublicState::kErrored;
  ClearClient();
  reader_ = nullptr;
}

}  // namespace mitto
