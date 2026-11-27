// The bytes of a Blob as a BytesConsumer, which Blob's stream() and a body of
// a Blob read, as Blink's BlobBytesConsumer. Chrome reads a blob from the
// browser into a data pipe that holds at most 2 MiB, and its bytes come from
// a later networking task each time the pipe has some; a Page holds the bytes
// itself, so a read waits for the same tasks. A body that is sent whole drains
// the bytes at once, as Blink's DrainAsFormData() hands the network service
// the blob.
//
// Upstream: third_party/blink/renderer/core/fetch/blob_bytes_consumer.h
// Upstream: third_party/blink/renderer/core/fetch/blob_bytes_consumer.cc
// Upstream: third_party/blink/renderer/platform/loader/fetch/data_pipe_bytes_consumer.cc

#ifndef MITTO_DOM_BLOB_BYTES_CONSUMER_H_
#define MITTO_DOM_BLOB_BYTES_CONSUMER_H_

#include <stddef.h>

#include <optional>
#include <string>

#include "base/memory/scoped_refptr.h"
#include "mitto/dom/blob_data.h"
#include "mitto/dom/bytes_consumer.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class ExecutionContext;

class BlobBytesConsumer final : public BytesConsumer {
 public:
  // The bytes of `handle`, whose tasks are `context`'s.
  BlobBytesConsumer(ExecutionContext* context,
                    scoped_refptr<BlobDataHandle> handle);
  ~BlobBytesConsumer() override;

  // BytesConsumer:
  Result BeginRead(base::span<const char>& buffer) override;
  Result EndRead(size_t read_size) override;
  std::optional<std::string> DrainAsBytes() override;
  scoped_refptr<BlobDataHandle> DrainAsBlobDataHandle() override;
  void SetClient(BytesConsumer::Client* client) override;
  void ClearClient() override;
  void Cancel() override;
  PublicState GetPublicState() const override;
  void Trace(cppgc::Visitor* visitor) const override;

 private:
  // The pipe has the next bytes, at most its capacity.
  void FillPipe();

  cppgc::WeakMember<ExecutionContext> context_;
  cppgc::Member<BytesConsumer::Client> client_;
  scoped_refptr<BlobDataHandle> handle_;
  // Whether a read has begun, after which the bytes cannot be drained.
  bool started_ = false;
  // Whether a task that fills the pipe is on its way.
  bool waiting_ = false;
  size_t pipe_capacity_ = 0;
  // How much has been read, and how much the pipe has given so far.
  size_t offset_ = 0;
  size_t available_ = 0;
  PublicState state_ = PublicState::kReadableOrWaiting;
};

}  // namespace mitto

#endif  // MITTO_DOM_BLOB_BYTES_CONSUMER_H_
