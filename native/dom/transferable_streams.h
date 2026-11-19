// The Streams Standard's cross-realm transform streams, as Blink's: a
// writable stream whose chunks go over a MessagePort to its peer, and a
// readable stream that gives what comes over its port, which a transferred
// stream is on each side. Each message is an object of a type and a value,
// "chunk", "pull", "close" or "error", which Blink numbers. Blink's in-process
// optimizers, which only some of its own streams have, are left out.
//
// Upstream: third_party/blink/renderer/core/streams/transferable_streams.h
// Upstream: third_party/blink/renderer/core/streams/transferable_streams.cc

#ifndef MITTO_DOM_TRANSFERABLE_STREAMS_H_
#define MITTO_DOM_TRANSFERABLE_STREAMS_H_

namespace v8 {
class Isolate;
}  // namespace v8

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class MessagePort;
class ReadableStream;
class WritableStream;

// SetUpCrossRealmTransformWritable() of a new stream, over `port`.
WritableStream* CreateCrossRealmTransformWritable(
    v8::Isolate* isolate,
    MessagePort* port,
    bindings::ExceptionState& exception_state);

// SetUpCrossRealmTransformReadable() of a new stream, over `port`.
ReadableStream* CreateCrossRealmTransformReadable(
    v8::Isolate* isolate,
    MessagePort* port,
    bindings::ExceptionState& exception_state);

}  // namespace mitto

#endif  // MITTO_DOM_TRANSFERABLE_STREAMS_H_
