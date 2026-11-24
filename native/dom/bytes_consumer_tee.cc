#include "mitto/dom/bytes_consumer_tee.h"

#include <string>
#include <utility>

#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/notreached.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/blob_bytes_consumer.h"
#include "mitto/dom/blob_data.h"
#include "mitto/dom/bytes_consumer.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/form_data_bytes_consumer.h"
#include "mitto/dom/heap_deque.h"
#include "mitto/dom/window_task.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-external-memory-accounter.h"
#include "v8/include/v8-isolate.h"

namespace mitto {
namespace {

class NoopClient final : public cppgc::GarbageCollected<NoopClient>,
                         public BytesConsumer::Client {
 public:
  void OnStateChange() override {}
};

// A piece of the source's bytes, which both destinations share, and which V8
// counts as memory outside its heap.
class Chunk final : public cppgc::GarbageCollected<Chunk> {
 public:
  explicit Chunk(base::span<const char> bytes)
      : isolate_(v8::Isolate::GetCurrent()),
        bytes_(bytes.begin(), bytes.end()) {
    accounter_.Increase(isolate_, bytes_.size());
  }
  ~Chunk() { accounter_.Decrease(isolate_, bytes_.size()); }

  base::span<const char> bytes() const { return bytes_; }
  size_t size() const { return bytes_.size(); }

  void Trace(cppgc::Visitor* visitor) const {}

 private:
  const raw_ptr<v8::Isolate> isolate_;
  const std::string bytes_;
  v8::ExternalMemoryAccounter accounter_;
};

class TeeHelper;

// What one branch reads: the pieces that the source gave, in order.
class Destination final : public BytesConsumer {
 public:
  Destination(ExecutionContext* execution_context, TeeHelper* tee)
      : execution_context_(execution_context), tee_(tee) {}

  Result BeginRead(base::span<const char>& buffer) override;
  Result EndRead(size_t read) override;

  void SetClient(BytesConsumer::Client* client) override {
    DCHECK(!client_);
    DCHECK(client);
    const PublicState state = GetPublicState();
    if (state == PublicState::kClosed || state == PublicState::kErrored) {
      return;
    }
    client_ = client;
  }

  void ClearClient() override { client_ = nullptr; }
  void Cancel() override;
  PublicState GetPublicState() const override;

  void Enqueue(Chunk* chunk) {
    if (is_cancelled_) {
      return;
    }
    chunks_.push_back(chunk);
  }

  bool IsEmpty() const { return chunks_.empty(); }

  void ClearChunks() {
    chunks_.clear();
    offset_ = 0;
  }

  void Notify();
  bool IsCancelled() const { return is_cancelled_; }

  void Trace(cppgc::Visitor* visitor) const override;

 private:
  void Close();

  cppgc::WeakMember<ExecutionContext> execution_context_;
  cppgc::Member<TeeHelper> tee_;
  cppgc::Member<BytesConsumer::Client> client_;
  HeapDeque<Chunk> chunks_;
  // The piece that a two-phase read holds.
  cppgc::Member<Chunk> chunk_in_use_;
  size_t offset_ = 0;
  bool is_cancelled_ = false;
  bool is_closed_ = false;
};

// Reads the source and gives each piece to both destinations.
class TeeHelper final : public cppgc::GarbageCollected<TeeHelper>,
                        public BytesConsumer::Client {
 public:
  TeeHelper(ExecutionContext* execution_context, BytesConsumer* consumer)
      : src_(consumer),
        destination1_(
            bindings::MakeGarbageCollected<Destination>(execution_context,
                                                        this)),
        destination2_(
            bindings::MakeGarbageCollected<Destination>(execution_context,
                                                        this)) {
    consumer->SetClient(this);
    // Neither destination has a client yet, so Notify() does nothing here.
    OnStateChange();
  }

  void OnStateChange() override {
    const bool destination1_was_empty = destination1_->IsEmpty();
    const bool destination2_was_empty = destination2_->IsEmpty();
    bool has_enqueued = false;
    while (true) {
      base::span<const char> buffer;
      auto result = src_->BeginRead(buffer);
      if (result == BytesConsumer::Result::kShouldWait) {
        if (has_enqueued && destination1_was_empty) {
          destination1_->Notify();
        }
        if (has_enqueued && destination2_was_empty) {
          destination2_->Notify();
        }
        return;
      }
      Chunk* chunk = nullptr;
      if (result == BytesConsumer::Result::kOk) {
        chunk = bindings::MakeGarbageCollected<Chunk>(buffer);
        result = src_->EndRead(buffer.size());
      }
      switch (result) {
        case BytesConsumer::Result::kOk:
          DCHECK(chunk);
          destination1_->Enqueue(chunk);
          destination2_->Enqueue(chunk);
          has_enqueued = true;
          break;
        case BytesConsumer::Result::kShouldWait:
          NOTREACHED();
        case BytesConsumer::Result::kDone:
          if (chunk) {
            destination1_->Enqueue(chunk);
            destination2_->Enqueue(chunk);
          }
          if (destination1_was_empty) {
            destination1_->Notify();
          }
          if (destination2_was_empty) {
            destination2_->Notify();
          }
          return;
        case BytesConsumer::Result::kError:
          ClearAndNotify();
          return;
      }
    }
  }

  BytesConsumer::PublicState GetPublicState() const {
    return src_->GetPublicState();
  }

  // Cancels the source once both destinations are cancelled.
  void Cancel() {
    if (!destination1_->IsCancelled() || !destination2_->IsCancelled()) {
      return;
    }
    src_->Cancel();
  }

  BytesConsumer* Destination1() const { return destination1_.Get(); }
  BytesConsumer* Destination2() const { return destination2_.Get(); }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(src_);
    visitor->Trace(destination1_);
    visitor->Trace(destination2_);
    BytesConsumer::Client::Trace(visitor);
  }

 private:
  void ClearAndNotify() {
    destination1_->ClearChunks();
    destination2_->ClearChunks();
    destination1_->Notify();
    destination2_->Notify();
  }

  const cppgc::Member<BytesConsumer> src_;
  const cppgc::Member<Destination> destination1_;
  const cppgc::Member<Destination> destination2_;
};

BytesConsumer::Result Destination::BeginRead(base::span<const char>& buffer) {
  DCHECK(!chunk_in_use_);
  buffer = {};
  if (is_cancelled_ || is_closed_) {
    return Result::kDone;
  }
  if (!chunks_.empty()) {
    Chunk* const chunk = chunks_.front();
    DCHECK_LE(offset_, chunk->size());
    buffer = chunk->bytes().subspan(offset_);
    chunk_in_use_ = chunk;
    return Result::kOk;
  }
  switch (tee_->GetPublicState()) {
    case PublicState::kReadableOrWaiting:
      return Result::kShouldWait;
    case PublicState::kClosed:
      is_closed_ = true;
      ClearClient();
      return Result::kDone;
    case PublicState::kErrored:
      ClearClient();
      return Result::kError;
  }
  NOTREACHED();
}

BytesConsumer::Result Destination::EndRead(size_t read) {
  DCHECK(chunk_in_use_);
  DCHECK(chunks_.empty() || chunk_in_use_ == chunks_.front());
  chunk_in_use_ = nullptr;
  if (chunks_.empty()) {
    // It errored during the two-phase read.
    DCHECK_EQ(PublicState::kErrored, GetPublicState());
    return Result::kOk;
  }
  Chunk* const chunk = chunks_.front();
  DCHECK_LE(offset_ + read, chunk->size());
  offset_ += read;
  if (chunk->size() == offset_) {
    offset_ = 0;
    chunks_.pop_front();
  }
  if (chunks_.empty() && tee_->GetPublicState() == PublicState::kClosed) {
    // All of it has been read.
    PostTaskInContext(execution_context_.Get(), TaskType::kNetworking,
                      base::BindOnce(
                          [](cppgc::Persistent<Destination> destination) {
                            destination->Close();
                          },
                          cppgc::Persistent<Destination>(this)));
  }
  return Result::kOk;
}

void Destination::Cancel() {
  DCHECK(!chunk_in_use_);
  const PublicState state = GetPublicState();
  if (state == PublicState::kClosed || state == PublicState::kErrored) {
    return;
  }
  is_cancelled_ = true;
  ClearChunks();
  ClearClient();
  tee_->Cancel();
}

BytesConsumer::PublicState Destination::GetPublicState() const {
  if (is_cancelled_ || is_closed_) {
    return PublicState::kClosed;
  }
  const PublicState state = tee_->GetPublicState();
  // Closed only once is_cancelled_ or is_closed_ says so.
  return state == PublicState::kClosed ? PublicState::kReadableOrWaiting
                                       : state;
}

void Destination::Notify() {
  if (is_cancelled_ || is_closed_) {
    return;
  }
  if (chunks_.empty() && tee_->GetPublicState() == PublicState::kClosed) {
    Close();
    return;
  }
  if (client_) {
    client_->OnStateChange();
    if (GetPublicState() == PublicState::kErrored) {
      ClearClient();
    }
  }
}

void Destination::Close() {
  // It may come from a task, after the destination closed another way.
  if (is_closed_ || is_cancelled_) {
    return;
  }
  DCHECK_EQ(PublicState::kClosed, tee_->GetPublicState());
  DCHECK(chunks_.empty());
  is_closed_ = true;
  if (client_) {
    client_->OnStateChange();
    ClearClient();
  }
}

void Destination::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(execution_context_);
  visitor->Trace(tee_);
  visitor->Trace(client_);
  chunks_.Trace(visitor);
  visitor->Trace(chunk_in_use_);
  BytesConsumer::Trace(visitor);
}

}  // namespace

void BytesConsumerTee(ExecutionContext* execution_context,
                      BytesConsumer* src,
                      BytesConsumer** dest1,
                      BytesConsumer** dest2) {
  if (scoped_refptr<BlobDataHandle> handle = src->DrainAsBlobDataHandle()) {
    // A client, to be consistent.
    src->SetClient(bindings::MakeGarbageCollected<NoopClient>());
    *dest1 = bindings::MakeGarbageCollected<BlobBytesConsumer>(
        execution_context, handle);
    *dest2 = bindings::MakeGarbageCollected<BlobBytesConsumer>(
        execution_context, handle);
    return;
  }
  if (std::optional<std::string> bytes = src->DrainAsBytes()) {
    src->SetClient(bindings::MakeGarbageCollected<NoopClient>());
    *dest1 = bindings::MakeGarbageCollected<FormDataBytesConsumer>(*bytes);
    *dest2 = bindings::MakeGarbageCollected<FormDataBytesConsumer>(
        std::move(*bytes));
    return;
  }
  TeeHelper* const tee =
      bindings::MakeGarbageCollected<TeeHelper>(execution_context, src);
  *dest1 = tee->Destination1();
  *dest2 = tee->Destination2();
}

}  // namespace mitto
