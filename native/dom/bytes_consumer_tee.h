// Splits a body's bytes into two consumers that each read all of them, as
// Blink's BytesConsumerTee() does for clone(): a blob that nothing has read
// goes to both as a blob, bytes that a script gave whole as those bytes, and
// any other source is read once, each piece kept until both have read it.
//
// Upstream: third_party/blink/renderer/core/fetch/bytes_consumer_tee.h
// Upstream: third_party/blink/renderer/core/fetch/bytes_consumer_tee.cc

#ifndef MITTO_DOM_BYTES_CONSUMER_TEE_H_
#define MITTO_DOM_BYTES_CONSUMER_TEE_H_

namespace mitto {

class BytesConsumer;
class ExecutionContext;

void BytesConsumerTee(ExecutionContext* execution_context,
                      BytesConsumer* src,
                      BytesConsumer** dest1,
                      BytesConsumer** dest2);

}  // namespace mitto

#endif  // MITTO_DOM_BYTES_CONSUMER_TEE_H_
