// Reads a fetch's body as soon as it comes, after a short delay, and holds it
// until something reads it, as Blink's BufferingBytesConsumer does, so that a
// body that no script reads does not hold the server back. It holds at most
// 55 MiB, as Chrome does with BufferedBytesConsumerLimitSize, and then waits
// for reads.
//
// Upstream: third_party/blink/renderer/platform/loader/fetch/buffering_bytes_consumer.h
// Upstream: third_party/blink/renderer/platform/loader/fetch/buffering_bytes_consumer.cc

#ifndef MITTO_DOM_BUFFERING_BYTES_CONSUMER_H_
#define MITTO_DOM_BUFFERING_BYTES_CONSUMER_H_

#include <stddef.h>

#include <deque>
#include <optional>
#include <string>

#include "mitto/dom/bytes_consumer.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

class BufferingBytesConsumer final : public BytesConsumer,
                                     public BytesConsumer::Client {
 public:
  static constexpr size_t kMaxBufferSize = 55 * 1024 * 1024;

  // One that starts buffering what `bytes_consumer` has after 50 ms, or at
  // the first read if that comes first.
  static BufferingBytesConsumer* CreateWithDelay(BytesConsumer* bytes_consumer);

  explicit BufferingBytesConsumer(BytesConsumer* bytes_consumer);
  ~BufferingBytesConsumer() override;

  // BytesConsumer:
  Result BeginRead(base::span<const char>& buffer) override;
  Result EndRead(size_t read_size) override;
  std::optional<std::string> DrainAsBytes() override;
  void SetClient(BytesConsumer::Client* client) override;
  void ClearClient() override;
  void Cancel() override;
  PublicState GetPublicState() const override;

  // BytesConsumer::Client:
  void OnStateChange() override;

  void Trace(cppgc::Visitor* visitor) const override;

 private:
  enum class BufferingState {
    kDelayed,
    kStarted,
    kStopped,
  };

  void OnTimerFired();
  void MaybeStartBuffering();
  void BufferData();

  const cppgc::Member<BytesConsumer> bytes_consumer_;
  cppgc::Member<BytesConsumer::Client> client_;
  std::deque<std::string> buffer_;
  size_t offset_for_first_chunk_ = 0;
  size_t total_buffer_size_ = 0;
  BufferingState buffering_state_ = BufferingState::kDelayed;
  bool has_seen_end_of_data_ = false;
  bool has_seen_error_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_BUFFERING_BYTES_CONSUMER_H_
