// The bytes of a body that a BodyLoader brings as it loads, a fetch's from the
// Session or a stored one of the Cache API's, as Blink's DataPipeBytesConsumer
// reads the data pipe that the network service fills: what has come waits
// until it is read, each read gives the loader credit for what it took, and
// the end comes once all of it has been read and the loader has said it is
// complete. Cancelling it, or its collection, lets the loader drop the rest,
// as closing the consumer end of a data pipe does.
//
// Upstream: third_party/blink/renderer/platform/loader/fetch/data_pipe_bytes_consumer.h
// Upstream: third_party/blink/renderer/platform/loader/fetch/data_pipe_bytes_consumer.cc

#ifndef MITTO_DOM_BODY_LOADER_BYTES_CONSUMER_H_
#define MITTO_DOM_BODY_LOADER_BYTES_CONSUMER_H_

#include <stddef.h>

#include <string>
#include <string_view>

#include "base/memory/raw_ptr.h"
#include "mitto/dom/bytes_consumer.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/prefinalizer.h"

namespace mitto {

class Window;

// What brings a body as it loads: a fetch's request, or a body that the Cache
// API stored.
class BodyLoader {
 public:
  // The body is read for the first time, which starts a stored body coming.
  virtual void BodyRequested() = 0;
  // `bytes` more of the body were read, for which its source gets credit.
  virtual void BodyTaken(size_t bytes) = 0;
  // The rest of the body is not wanted: what reads it cancelled it or was
  // collected. Called during garbage collection too, which may not run
  // scripts or free handles, so the loader stops from a later task.
  virtual void BodyDropped() = 0;

 protected:
  virtual ~BodyLoader() = default;
};

class BodyLoaderBytesConsumer final : public BytesConsumer {
  CPPGC_USING_PRE_FINALIZER(BodyLoaderBytesConsumer, Dispose);

 public:
  // The body that `loader` brings, read in `window`'s context, in which what
  // it tells its client later runs.
  BodyLoaderBytesConsumer(BodyLoader* loader, Window* window);
  ~BodyLoaderBytesConsumer() override;

  // What its loader tells it.
  void AppendBody(std::string_view bytes);
  // All of the body has come, as DataPipeBytesConsumer's SignalComplete()
  // and the end of its pipe together say.
  void SignalComplete();
  void SignalError();
  // The loader cancelled its source before the end: the body fails from a
  // later task, as a data pipe that its producer closed early shows it.
  void SignalErrorLater();
  // The loader has gone, and tells it nothing more.
  void DetachLoader() { loader_ = nullptr; }

  // BytesConsumer:
  Result BeginRead(base::span<const char>& buffer) override;
  Result EndRead(size_t read_size) override;
  void SetClient(BytesConsumer::Client* client) override;
  void ClearClient() override;
  void Cancel() override;
  PublicState GetPublicState() const override;

  void Trace(cppgc::Visitor* visitor) const override;

 private:
  bool IsWaiting() const { return state_ == InternalState::kWaiting; }
  size_t Unread() const { return buffer_.size() - offset_; }
  // Closes it once it has all of the body and none of it is left to read.
  void MaybeClose();
  void SetError();
  // What the watcher of a data pipe does once the pipe signals.
  void Notify();
  void ScheduleNotify();
  void Dispose();

  raw_ptr<BodyLoader> loader_;
  const cppgc::WeakMember<Window> window_;
  // What has come and not been read: the bytes of `buffer_` from `offset_`.
  std::string buffer_;
  size_t offset_ = 0;
  bool requested_ = false;
  bool completion_signaled_ = false;
  bool is_in_two_phase_read_ = false;
  bool has_pending_complete_ = false;
  bool has_pending_error_ = false;
  bool has_pending_notification_ = false;
  InternalState state_ = InternalState::kWaiting;
  cppgc::Member<BytesConsumer::Client> client_;
};

}  // namespace mitto

#endif  // MITTO_DOM_BODY_LOADER_BYTES_CONSUMER_H_
