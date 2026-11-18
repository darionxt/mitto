// The bytes of a ReadableStream that a script made, read with a default reader
// of its own, as Blink's ReadableStreamBytesConsumer reads a body made from a
// stream: each chunk must be a Uint8Array, and anything else errors it.
//
// Upstream: third_party/blink/renderer/core/fetch/readable_stream_bytes_consumer.h
// Upstream: third_party/blink/renderer/core/fetch/readable_stream_bytes_consumer.cc

#ifndef MITTO_DOM_READABLE_STREAM_BYTES_CONSUMER_H_
#define MITTO_DOM_READABLE_STREAM_BYTES_CONSUMER_H_

#include <stddef.h>

#include "base/memory/raw_ptr.h"
#include "mitto/dom/bytes_consumer.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/v8-traced-handle.h"
#include "v8/include/v8-typed-array.h"

namespace v8 {
class Isolate;
}  // namespace v8

namespace mitto {

class ReadableStream;
class ReadableStreamDefaultReader;

class ReadableStreamBytesConsumer final : public BytesConsumer {
 public:
  // Locks `stream`, which must not be locked, to a reader of its own.
  ReadableStreamBytesConsumer(v8::Isolate* isolate, ReadableStream* stream);
  ~ReadableStreamBytesConsumer() override;

  // BytesConsumer:
  Result BeginRead(base::span<const char>& buffer) override;
  Result EndRead(size_t read_size) override;
  void SetClient(Client* client) override;
  void ClearClient() override;
  void Cancel() override;
  PublicState GetPublicState() const override;

  void Trace(cppgc::Visitor* visitor) const override;

 private:
  class BytesConsumerReadRequest;

  void OnRead(v8::Local<v8::Uint8Array> buffer);
  void OnReadDone();
  void OnRejected();
  void SetErrored();

  const raw_ptr<v8::Isolate> isolate_;
  cppgc::Member<ReadableStreamDefaultReader> reader_;
  cppgc::Member<Client> client_;
  // The chunk that is being read, and how much of it has been.
  v8::TracedReference<v8::Uint8Array> pending_buffer_;
  size_t pending_offset_ = 0;
  PublicState state_ = PublicState::kReadableOrWaiting;
  bool is_reading_ = false;
  bool is_inside_read_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_READABLE_STREAM_BYTES_CONSUMER_H_
