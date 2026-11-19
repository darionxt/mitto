// A source of bytes read in two phases, as Blink's BytesConsumer: BeginRead()
// gives what can be read now, and EndRead() says how much of it was taken. A
// body's stream, and the loaders that read a whole body, read through one.
//
// Upstream: third_party/blink/renderer/platform/loader/fetch/bytes_consumer.h
// Upstream: third_party/blink/renderer/platform/loader/fetch/bytes_consumer.cc

#ifndef MITTO_DOM_BYTES_CONSUMER_H_
#define MITTO_DOM_BYTES_CONSUMER_H_

#include <stddef.h>

#include <optional>
#include <string>

#include "base/containers/span.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class BytesConsumer : public cppgc::GarbageCollected<BytesConsumer> {
 public:
  enum class Result {
    kOk,
    kShouldWait,
    kDone,
    kError,
  };

  // Readable and waiting are one state here: a consumer that has nothing to
  // give returns kShouldWait from BeginRead(), and its client hears when that
  // changes.
  enum class PublicState {
    kReadableOrWaiting,
    kClosed,
    kErrored,
  };

  // What hears that a consumer's state changed, or that it has something to
  // read.
  class Client : public cppgc::GarbageCollectedMixin {
   public:
    virtual ~Client() = default;
    virtual void OnStateChange() = 0;
  };

  BytesConsumer() = default;
  BytesConsumer(const BytesConsumer&) = delete;
  BytesConsumer& operator=(const BytesConsumer&) = delete;
  virtual ~BytesConsumer() = default;

  // Gives in `buffer` what can be read now, with kOk; kShouldWait when there
  // is nothing yet, kDone at the end and kError on failure, with `buffer`
  // empty. After kOk, EndRead() must come before anything else.
  [[nodiscard]] virtual Result BeginRead(base::span<const char>& buffer) = 0;
  // Takes `read_size` bytes of what BeginRead() gave.
  [[nodiscard]] virtual Result EndRead(size_t read_size) = 0;

  // All of its bytes at once, for a consumer that holds them, as Blink's
  // DrainAsFormData() gives them, after which it is closed; nullopt for one
  // that does not, which is left as it was.
  virtual std::optional<std::string> DrainAsBytes();

  // Sets the client, which must not be set already.
  virtual void SetClient(Client* client) = 0;
  virtual void ClearClient() = 0;

  // Nothing more is wanted: it closes, and lets its source go.
  virtual void Cancel() = 0;

  virtual PublicState GetPublicState() const = 0;

  // One that is closed and empty, and one that has failed.
  static BytesConsumer* CreateClosed();
  static BytesConsumer* CreateErrored();

  virtual void Trace(cppgc::Visitor* visitor) const {}

 protected:
  enum class InternalState {
    kWaiting,
    kClosed,
    kErrored,
  };

  static PublicState GetPublicStateFromInternalState(InternalState state);
};

}  // namespace mitto

#endif  // MITTO_DOM_BYTES_CONSUMER_H_
