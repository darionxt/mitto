// The Streams Standard's CountQueuingStrategy, as Blink's: a highWaterMark, and the
// size function that each global object shares.
//
// Upstream: third_party/blink/renderer/core/streams/count_queuing_strategy.h
// Upstream: third_party/blink/renderer/core/streams/count_queuing_strategy.cc

#ifndef MITTO_DOM_COUNT_QUEUING_STRATEGY_H_
#define MITTO_DOM_COUNT_QUEUING_STRATEGY_H_

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/v8-local-handle.h"

namespace v8 {
class Value;
}  // namespace v8

namespace mitto {

namespace bindings {
struct QueuingStrategyInit;
}  // namespace bindings

class CountQueuingStrategy final : public bindings::ScriptWrappable {
 public:
  static CountQueuingStrategy* Create(const bindings::QueuingStrategyInit& init);

  explicit CountQueuingStrategy(double high_water_mark);
  ~CountQueuingStrategy() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  double HighWaterMark() const { return high_water_mark_; }
  v8::Local<v8::Value> Size() const;

 private:
  const double high_water_mark_;
};

}  // namespace mitto

#endif  // MITTO_DOM_COUNT_QUEUING_STRATEGY_H_
