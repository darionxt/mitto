// The Streams Standard's ByteLengthQueuingStrategy, as Blink's: a highWaterMark, and the
// size function that each global object shares.
//
// Upstream: third_party/blink/renderer/core/streams/byte_length_queuing_strategy.h
// Upstream: third_party/blink/renderer/core/streams/byte_length_queuing_strategy.cc

#ifndef MITTO_DOM_BYTE_LENGTH_QUEUING_STRATEGY_H_
#define MITTO_DOM_BYTE_LENGTH_QUEUING_STRATEGY_H_

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/v8-local-handle.h"

namespace v8 {
class Value;
}  // namespace v8

namespace mitto {

namespace bindings {
struct QueuingStrategyInit;
}  // namespace bindings

class ByteLengthQueuingStrategy final : public bindings::ScriptWrappable {
 public:
  static ByteLengthQueuingStrategy* Create(const bindings::QueuingStrategyInit& init);

  explicit ByteLengthQueuingStrategy(double high_water_mark);
  ~ByteLengthQueuingStrategy() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  double HighWaterMark() const { return high_water_mark_; }
  v8::Local<v8::Value> Size() const;

 private:
  const double high_water_mark_;
};

}  // namespace mitto

#endif  // MITTO_DOM_BYTE_LENGTH_QUEUING_STRATEGY_H_
