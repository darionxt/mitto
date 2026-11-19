#include "mitto/dom/transferable_streams.h"

#include <array>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "base/memory/raw_ptr.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/script_function.h"
#include "mitto/bindings/script_promise.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_message_event.h"
#include "mitto/dom/message_event.h"
#include "mitto/dom/message_port.h"
#include "mitto/dom/miscellaneous_operations.h"
#include "mitto/dom/readable_stream.h"
#include "mitto/dom/readable_stream_default_controller.h"
#include "mitto/dom/stream_algorithms.h"
#include "mitto/dom/writable_stream.h"
#include "mitto/dom/writable_stream_default_controller.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function-callback.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-traced-handle.h"

namespace mitto {
namespace {

// These are the types of messages that are sent between peers.
enum class MessageType { kPull, kChunk, kClose, kError };

// Creates a JavaScript object with a null prototype structured like {key1:
// value1, key2: value2}. This is used to create objects to be serialized by
// postMessage.
v8::Local<v8::Object> CreateKeyValueObject(v8::Isolate* isolate,
                                           const char* key1,
                                           v8::Local<v8::Value> value1,
                                           const char* key2,
                                           v8::Local<v8::Value> value2) {
  std::array<v8::Local<v8::Name>, 2> names = {
      v8::String::NewFromUtf8(isolate, key1, v8::NewStringType::kInternalized)
          .ToLocalChecked(),
      v8::String::NewFromUtf8(isolate, key2, v8::NewStringType::kInternalized)
          .ToLocalChecked()};
  std::array<v8::Local<v8::Value>, 2> values = {value1, value2};
  return v8::Object::New(isolate, v8::Null(isolate), names.data(),
                         values.data(), names.size());
}

// Unpacks an object created by CreateKeyValueObject(). |value1| and |value2|
// are out parameters. Returns false on failure.
bool UnpackKeyValueObject(v8::Isolate* isolate,
                          v8::Local<v8::Object> object,
                          const char* key1,
                          v8::Local<v8::Value>* value1,
                          const char* key2,
                          v8::Local<v8::Value>* value2) {
  const v8::TryCatch try_catch(isolate);
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  if (!object
           ->Get(context, v8::String::NewFromUtf8(
                              isolate, key1, v8::NewStringType::kInternalized)
                              .ToLocalChecked())
           .ToLocal(value1)) {
    return false;
  }
  if (!object
           ->Get(context, v8::String::NewFromUtf8(
                              isolate, key2, v8::NewStringType::kInternalized)
                              .ToLocalChecked())
           .ToLocal(value2)) {
    return false;
  }
  return true;
}

// Sends a message with type |type| and contents |value| over |port|. The type
// is packed as a number with key "t", and the value is packed with key "v".
void PackAndPostMessage(v8::Isolate* isolate,
                        MessagePort* port,
                        MessageType type,
                        v8::Local<v8::Value> value,
                        bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#abstract-opdef-packandpostmessage
  // 1. Let message be OrdinaryObjectCreate(null).
  // 2. Perform ! CreateDataProperty(message, "type", type).
  // 3. Perform ! CreateDataProperty(message, "value", value).
  const v8::Local<v8::Object> packed = CreateKeyValueObject(
      isolate, "t", v8::Number::New(isolate, static_cast<int>(type)), "v",
      value);
  // 4. Let targetPort be the port with which port is entangled, if any;
  //    otherwise let it be null.
  // 5. Let options be «[ "transfer" → « » ]».
  // 6. Run the message port post message steps providing targetPort, message,
  //    and options.
  port->PostMessage(packed, std::vector<v8::Local<v8::Object>>(),
                    exception_state);
}

// Sends a kError message to the remote side, disregarding failure.
void CrossRealmTransformSendError(v8::Isolate* isolate,
                                  MessagePort* port,
                                  v8::Local<v8::Value> error) {
  const v8::TryCatch try_catch(isolate);
  // https://streams.spec.whatwg.org/#abstract-opdef-crossrealmtransformsenderror
  // 1. Perform PackAndPostMessage(port, "error", error), discarding the result.
  bindings::ExceptionState pass_through(isolate);
  PackAndPostMessage(isolate, port, MessageType::kError, error, pass_through);
}

// Same as PackAndPostMessage(), except that it attempts to handle exceptions by
// sending a kError message to the remote side. Any error from sending the
// kError message is ignored. Returns true for a normal completion and false
// for an abrupt completion, whose value goes in |error|.
bool PackAndPostMessageHandlingError(v8::Isolate* isolate,
                                     MessagePort* port,
                                     MessageType type,
                                     v8::Local<v8::Value> value,
                                     v8::Local<v8::Value>* error) {
  const v8::TryCatch try_catch(isolate);
  // https://streams.spec.whatwg.org/#abstract-opdef-packandpostmessagehandlingerror
  // 1. Let result be PackAndPostMessage(port, type, value).
  bindings::ExceptionState pass_through(isolate);
  PackAndPostMessage(isolate, port, type, value, pass_through);
  // 2. If result is an abrupt completion,
  if (try_catch.HasCaught()) {
    //   1. Perform ! CrossRealmTransformSendError(port, result.[[Value]]).
    // 3. Return result as a completion record.
    *error = try_catch.Exception();
    CrossRealmTransformSendError(isolate, port, try_catch.Exception());
    return false;
  }
  return true;
}

// Base class for CrossRealmTransformWritable and CrossRealmTransformReadable.
// Contains common methods that are used when handling MessagePort events.
class CrossRealmTransformStream
    : public cppgc::GarbageCollected<CrossRealmTransformStream> {
 public:
  virtual ~CrossRealmTransformStream() = default;

  virtual MessagePort* GetMessagePort() const = 0;

  // HandleMessage() is called by CrossRealmTransformMessageListener to handle
  // an incoming message from the MessagePort.
  virtual void HandleMessage(MessageType type, v8::Local<v8::Value> value) = 0;

  // HandleError() is called by CrossRealmTransformErrorListener when an error
  // event is fired on the message port. It should error the stream.
  virtual void HandleError(v8::Local<v8::Value> error) = 0;

  virtual void Trace(cppgc::Visitor* visitor) const {}
};

// Handles MessageEvents from the MessagePort, as Blink's native listener does.
class CrossRealmTransformMessageListener final
    : public bindings::ScriptFunction {
 public:
  explicit CrossRealmTransformMessageListener(CrossRealmTransformStream* target)
      : target_(target) {}

  void CallRaw(const v8::FunctionCallbackInfo<v8::Value>& info) override {
    v8::Isolate* const isolate = info.GetIsolate();
    MessageEvent* const message =
        info.Length() > 0
            ? bindings::V8MessageEvent::ToWrappable(isolate, info[0])
            : nullptr;
    if (!message) {
      return;
    }
    // Common to SetUpCrossRealmTransformReadable() and
    // SetUpCrossRealmTransformWritable().
    // 1. Let data be the data of the message.
    const v8::Local<v8::Value> data = message->Data();
    // 2. Assert: Type(data) is Object.
    // In the real world, the data could come from a compromised peer.
    if (!data->IsObject()) {
      return;
    }
    // 3. Let type be ! Get(data, "type").
    // 4. Let value be ! Get(data, "value").
    v8::Local<v8::Value> type;
    v8::Local<v8::Value> value;
    if (!UnpackKeyValueObject(isolate, data.As<v8::Object>(), "t", &type, "v",
                              &value)) {
      return;
    }
    // 5. Assert: Type(type) is String
    // This implementation uses numbers for types rather than strings.
    if (!type->IsNumber()) {
      return;
    }
    const int type_value = static_cast<int>(type.As<v8::Number>()->Value());
    target_->HandleMessage(static_cast<MessageType>(type_value), value);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(target_);
    bindings::ScriptFunction::Trace(visitor);
  }

 private:
  const cppgc::Member<CrossRealmTransformStream> target_;
};

// Handles "messageerror" events from the MessagePort.
class CrossRealmTransformErrorListener final : public bindings::ScriptFunction {
 public:
  explicit CrossRealmTransformErrorListener(CrossRealmTransformStream* target)
      : target_(target) {}

  void CallRaw(const v8::FunctionCallbackInfo<v8::Value>& info) override {
    v8::Isolate* const isolate = info.GetIsolate();
    // 1. Let error be a new "DataCloneError" DOMException.
    const v8::Local<v8::Value> error =
        bindings::PerIsolateData::From(isolate)->dom_exception_hooks().create(
            isolate, "DataCloneError", "chunk could not be cloned");
    // 2. Perform ! CrossRealmTransformSendError(port, error).
    MessagePort* const message_port = target_->GetMessagePort();
    CrossRealmTransformSendError(isolate, message_port, error);
    // 4. Disentangle port.
    message_port->Close();
    target_->HandleError(error);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(target_);
    bindings::ScriptFunction::Trace(visitor);
  }

 private:
  const cppgc::Member<CrossRealmTransformStream> target_;
};

// Listens on `port` for `stream`, as Blink's setOnmessage() and
// setOnmessageerror() with native listeners do, which start the port.
void Listen(v8::Isolate* isolate,
            MessagePort* port,
            CrossRealmTransformStream* stream) {
  port->SetAttributeEventHandler(
      isolate, u"message",
      bindings::MakeGarbageCollected<CrossRealmTransformMessageListener>(stream)
          ->ToV8Function(isolate));
  port->SetAttributeEventHandler(
      isolate, u"messageerror",
      bindings::MakeGarbageCollected<CrossRealmTransformErrorListener>(stream)
          ->ToV8Function(isolate));
  port->Start();
}

// Class for data associated with the writable side of the cross realm transform
// stream.
class CrossRealmTransformWritable final : public CrossRealmTransformStream {
 public:
  CrossRealmTransformWritable(v8::Isolate* isolate, MessagePort* port)
      : isolate_(isolate),
        message_port_(port),
        backpressure_promise_(
            bindings::MakeGarbageCollected<bindings::ScriptPromiseResolver>(
                isolate)) {}

  WritableStream* CreateWritableStream(
      bindings::ExceptionState& exception_state);

  MessagePort* GetMessagePort() const override { return message_port_.Get(); }
  void HandleMessage(MessageType type, v8::Local<v8::Value> value) override;
  void HandleError(v8::Local<v8::Value> error) override;

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(message_port_);
    visitor->Trace(backpressure_promise_);
    visitor->Trace(controller_);
    CrossRealmTransformStream::Trace(visitor);
  }

 private:
  class WriteAlgorithm;
  class CloseAlgorithm;
  class AbortAlgorithm;

  const raw_ptr<v8::Isolate> isolate_;
  const cppgc::Member<MessagePort> message_port_;
  cppgc::Member<bindings::ScriptPromiseResolver> backpressure_promise_;
  cppgc::Member<WritableStreamDefaultController> controller_;
};

class CrossRealmTransformWritable::WriteAlgorithm final
    : public StreamAlgorithm {
 public:
  explicit WriteAlgorithm(CrossRealmTransformWritable* writable)
      : writable_(writable) {}

  // Sends the chunk to the readable side, possibly after waiting for
  // backpressure.
  v8::Local<v8::Promise> Run(v8::Isolate* isolate,
                             base::span<v8::Local<v8::Value>> argv) override {
    // https://streams.spec.whatwg.org/#abstract-opdef-setupcrossrealmtransformwritable
    // 8. Let writeAlgorithm be the following steps, taking a chunk argument:
    DCHECK_EQ(argv.size(), 1u);
    const v8::Local<v8::Value> chunk = argv[0];
    // 1. If backpressurePromise is undefined, set backpressurePromise to a
    //    promise resolved with undefined.
    // As an optimization for the common case, we call DoWrite() synchronously
    // instead. The difference is not observable.
    if (!writable_->backpressure_promise_) {
      return DoWrite(isolate, chunk);
    }
    // 2. Return the result of reacting to backpressurePromise with the
    //    following fulfillment steps:
    return bindings::PromiseThen(
        isolate, writable_->backpressure_promise_->Promise(isolate),
        bindings::MakeGarbageCollected<DoWriteOnResolve>(isolate, chunk, this));
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(writable_);
    StreamAlgorithm::Trace(visitor);
  }

 private:
  // A promise handler which calls DoWrite() when the promise resolves.
  class DoWriteOnResolve final : public bindings::ScriptFunction {
   public:
    DoWriteOnResolve(v8::Isolate* isolate,
                     v8::Local<v8::Value> chunk,
                     WriteAlgorithm* target)
        : chunk_(isolate, chunk), target_(target) {}

    v8::Local<v8::Value> Call(v8::Isolate* isolate,
                              v8::Local<v8::Value>) override {
      return target_->DoWrite(isolate, chunk_.Get(isolate));
    }

    void Trace(cppgc::Visitor* visitor) const override {
      visitor->Trace(chunk_);
      visitor->Trace(target_);
      bindings::ScriptFunction::Trace(visitor);
    }

   private:
    const v8::TracedReference<v8::Value> chunk_;
    const cppgc::Member<WriteAlgorithm> target_;
  };

  // Sends a chunk over the message port to the readable side.
  v8::Local<v8::Promise> DoWrite(v8::Isolate* isolate,
                                 v8::Local<v8::Value> chunk) {
    // 2. Return the result of reacting to backpressurePromise with the
    //    following fulfillment steps:
    //   1. Set backpressurePromise to a new promise.
    writable_->backpressure_promise_ =
        bindings::MakeGarbageCollected<bindings::ScriptPromiseResolver>(
            isolate);
    v8::Local<v8::Value> error;
    //   2. Let result be PackAndPostMessageHandlingError(port, "chunk",
    //      chunk).
    const bool success = PackAndPostMessageHandlingError(
        isolate, writable_->message_port_, MessageType::kChunk, chunk, &error);
    //   3. If result is an abrupt completion,
    if (!success) {
      //   1. Disentangle port.
      writable_->message_port_->Close();
      //   2. Return a promise rejected with result.[[Value]].
      return bindings::RejectedPromise(isolate, error);
    }
    //   4. Otherwise, return a promise resolved with undefined.
    return bindings::ResolvedUndefinedPromise(isolate);
  }

  const cppgc::Member<CrossRealmTransformWritable> writable_;
};

class CrossRealmTransformWritable::CloseAlgorithm final
    : public StreamAlgorithm {
 public:
  explicit CloseAlgorithm(CrossRealmTransformWritable* writable)
      : writable_(writable) {}

  // Sends a close message to the readable side and closes the message port.
  v8::Local<v8::Promise> Run(v8::Isolate* isolate,
                             base::span<v8::Local<v8::Value>> argv) override {
    DCHECK_EQ(argv.size(), 0u);
    // 9. Let closeAlgorithm be the folowing steps:
    v8::Local<v8::Value> error;
    //   1. Perform ! PackAndPostMessage(port, "close", undefined).
    // In the standard, this can't fail. However, in the implementation failure
    // is possible, so we have to handle it.
    const bool success = PackAndPostMessageHandlingError(
        isolate, writable_->message_port_, MessageType::kClose,
        v8::Undefined(isolate), &error);
    //   2. Disentangle port.
    writable_->message_port_->Close();
    // Error the stream if an error occurred.
    if (!success) {
      return bindings::RejectedPromise(isolate, error);
    }
    //   3. Return a promise resolved with undefined.
    return bindings::ResolvedUndefinedPromise(isolate);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(writable_);
    StreamAlgorithm::Trace(visitor);
  }

 private:
  const cppgc::Member<CrossRealmTransformWritable> writable_;
};

class CrossRealmTransformWritable::AbortAlgorithm final
    : public StreamAlgorithm {
 public:
  explicit AbortAlgorithm(CrossRealmTransformWritable* writable)
      : writable_(writable) {}

  // Sends an abort message to the readable side and closes the message port.
  v8::Local<v8::Promise> Run(v8::Isolate* isolate,
                             base::span<v8::Local<v8::Value>> argv) override {
    // 10. Let abortAlgorithm be the following steps, taking a reason argument:
    DCHECK_EQ(argv.size(), 1u);
    const v8::Local<v8::Value> reason = argv[0];
    v8::Local<v8::Value> error;
    //   1. Let result be PackAndPostMessageHandlingError(port, "error",
    //      reason).
    const bool success = PackAndPostMessageHandlingError(
        isolate, writable_->message_port_, MessageType::kError, reason, &error);
    //   2. Disentangle port.
    writable_->message_port_->Close();
    //   3. If result is an abrupt completion, return a promise rejected with
    //      result.[[Value]].
    if (!success) {
      return bindings::RejectedPromise(isolate, error);
    }
    //   4. Otherwise, return a promise resolved with undefined.
    return bindings::ResolvedUndefinedPromise(isolate);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(writable_);
    StreamAlgorithm::Trace(visitor);
  }

 private:
  const cppgc::Member<CrossRealmTransformWritable> writable_;
};

WritableStream* CrossRealmTransformWritable::CreateWritableStream(
    bindings::ExceptionState& exception_state) {
  DCHECK(!controller_) << "CreateWritableStream() can only be called once";
  // https://streams.spec.whatwg.org/#abstract-opdef-setupcrossrealmtransformwritable
  // The order of operations is significantly different from the standard, but
  // functionally equivalent.
  //  3. Let backpressurePromise be a new promise.
  // |backpressure_promise_| is initialized by the constructor.
  //  4. Add a handler for port’s message event with the following steps:
  //  5. Add a handler for port’s messageerror event with the following steps:
  //  6. Enable port’s port message queue.
  Listen(isolate_, message_port_, this);
  //  1. Perform ! InitializeWritableStream(stream).
  //  2. Let controller be a new WritableStreamDefaultController.
  //  7. Let startAlgorithm be an algorithm that returns undefined.
  // 11. Let sizeAlgorithm be an algorithm that returns 1.
  // 12. Perform ! SetUpWritableStreamDefaultController(stream, controller,
  //     startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm, 1,
  //     sizeAlgorithm).
  WritableStream* const stream = WritableStream::Create(
      isolate_, CreateTrivialStartAlgorithm(),
      bindings::MakeGarbageCollected<WriteAlgorithm>(this),
      bindings::MakeGarbageCollected<CloseAlgorithm>(this),
      bindings::MakeGarbageCollected<AbortAlgorithm>(this), 1,
      CreateDefaultSizeAlgorithm(), exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  controller_ = stream->Controller();
  return stream;
}

void CrossRealmTransformWritable::HandleMessage(MessageType type,
                                                v8::Local<v8::Value> value) {
  // 4. Add a handler for port’s message event with the following steps:
  // The initial steps are done by CrossRealmTransformMessageListener
  switch (type) {
    // 6. If type is "pull",
    case MessageType::kPull:
      // 1. If backpressurePromise is not undefined,
      if (backpressure_promise_) {
        // 1. Resolve backpressurePromise with undefined.
        backpressure_promise_->Resolve(isolate_);
        // 2. Set backpressurePromise to undefined.
        backpressure_promise_ = nullptr;
      }
      return;
    // 7. Otherwise if type is "error",
    case MessageType::kError:
      // 1. Perform ! WritableStreamDefaultControllerErrorIfNeeded(controller,
      //    value).
      WritableStreamDefaultController::ErrorIfNeeded(isolate_, controller_,
                                                     value);
      // 2. If backpressurePromise is not undefined,
      if (backpressure_promise_) {
        // 1. Resolve backpressurePromise with undefined.
        // 2. Set backpressurePromise to undefined.
        backpressure_promise_->Resolve(isolate_);
        backpressure_promise_ = nullptr;
      }
      return;
    default:
      // An invalid message from the peer is ignored.
      return;
  }
}

void CrossRealmTransformWritable::HandleError(v8::Local<v8::Value> error) {
  // 5. Add a handler for port’s messageerror event with the following steps:
  // The first two steps, and the last step, are performed by
  // CrossRealmTransformErrorListener.
  //   3. Perform ! WritableStreamDefaultControllerError(controller, error).
  WritableStreamDefaultController::ErrorIfNeeded(isolate_, controller_, error);
}

// Class for data associated with the readable side of the cross realm transform
// stream.
class CrossRealmTransformReadable final : public CrossRealmTransformStream {
 public:
  CrossRealmTransformReadable(v8::Isolate* isolate, MessagePort* port)
      : isolate_(isolate), message_port_(port) {}

  ReadableStream* CreateReadableStream(
      bindings::ExceptionState& exception_state);

  MessagePort* GetMessagePort() const override { return message_port_.Get(); }
  void HandleMessage(MessageType type, v8::Local<v8::Value> value) override;
  void HandleError(v8::Local<v8::Value> error) override;

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(message_port_);
    visitor->Trace(controller_);
    CrossRealmTransformStream::Trace(visitor);
  }

 private:
  class PullAlgorithm;
  class CancelAlgorithm;

  const raw_ptr<v8::Isolate> isolate_;
  const cppgc::Member<MessagePort> message_port_;
  cppgc::Member<ReadableStreamDefaultController> controller_;
};

class CrossRealmTransformReadable::PullAlgorithm final
    : public StreamAlgorithm {
 public:
  explicit PullAlgorithm(CrossRealmTransformReadable* readable)
      : readable_(readable) {}

  // Sends a pull message to the writable side and then waits for backpressure
  // to clear.
  v8::Local<v8::Promise> Run(v8::Isolate* isolate,
                             base::span<v8::Local<v8::Value>> argv) override {
    DCHECK_EQ(argv.size(), 0u);
    // https://streams.spec.whatwg.org/#abstract-opdef-setupcrossrealmtransformreadable
    // 7. Let pullAlgorithm be the following steps:
    v8::Local<v8::Value> error;
    //   1. Perform ! PackAndPostMessage(port, "pull", undefined).
    // In the standard this can't throw an exception, but in the implementation
    // it can, so we need to be able to handle it.
    const bool success = PackAndPostMessageHandlingError(
        isolate, readable_->message_port_, MessageType::kPull,
        v8::Undefined(isolate), &error);
    if (!success) {
      readable_->message_port_->Close();
      return bindings::RejectedPromise(isolate, error);
    }
    //   2. Return a promise resolved with undefined.
    // The Streams Standard guarantees that PullAlgorithm won't be called again
    // until Enqueue() is called.
    return bindings::ResolvedUndefinedPromise(isolate);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(readable_);
    StreamAlgorithm::Trace(visitor);
  }

 private:
  const cppgc::Member<CrossRealmTransformReadable> readable_;
};

class CrossRealmTransformReadable::CancelAlgorithm final
    : public StreamAlgorithm {
 public:
  explicit CancelAlgorithm(CrossRealmTransformReadable* readable)
      : readable_(readable) {}

  // Sends a cancel message to the writable side and closes the message port.
  v8::Local<v8::Promise> Run(v8::Isolate* isolate,
                             base::span<v8::Local<v8::Value>> argv) override {
    // 8. Let cancelAlgorithm be the following steps, taking a reason argument:
    DCHECK_EQ(argv.size(), 1u);
    const v8::Local<v8::Value> reason = argv[0];
    v8::Local<v8::Value> error;
    //   1. Let result be PackAndPostMessageHandlingError(port, "error",
    //      reason).
    const bool success = PackAndPostMessageHandlingError(
        isolate, readable_->message_port_, MessageType::kError, reason, &error);
    //   2. Disentangle port.
    readable_->message_port_->Close();
    //   3. If result is an abrupt completion, return a promise rejected with
    //      result.[[Value]].
    if (!success) {
      return bindings::RejectedPromise(isolate, error);
    }
    //   4. Otherwise, return a promise resolved with undefined.
    return bindings::ResolvedUndefinedPromise(isolate);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(readable_);
    StreamAlgorithm::Trace(visitor);
  }

 private:
  const cppgc::Member<CrossRealmTransformReadable> readable_;
};

ReadableStream* CrossRealmTransformReadable::CreateReadableStream(
    bindings::ExceptionState& exception_state) {
  DCHECK(!controller_) << "CreateReadableStream can only be called once";
  // https://streams.spec.whatwg.org/#abstract-opdef-setupcrossrealmtransformreadable
  // The order of operations is significantly different from the standard, but
  // functionally equivalent.
  //  3. Add a handler for port’s message event with the following steps:
  //  4. Add a handler for port’s messageerror event with the following steps:
  //  5. Enable port’s port message queue.
  Listen(isolate_, message_port_, this);
  //  6. Let startAlgorithm be an algorithm that returns undefined.
  //  7. Let pullAlgorithm be the following steps:
  //  8. Let cancelAlgorithm be the following steps, taking a reason argument:
  //  9. Let sizeAlgorithm be an algorithm that returns 1.
  // 10. Perform ! SetUpReadableStreamDefaultController(stream, controller,
  //     startAlgorithm, pullAlgorithm, cancelAlgorithm, 0, sizeAlgorithm).
  ReadableStream* const stream = ReadableStream::Create(
      isolate_, CreateTrivialStartAlgorithm(),
      bindings::MakeGarbageCollected<PullAlgorithm>(this),
      bindings::MakeGarbageCollected<CancelAlgorithm>(this),
      /*high_water_mark=*/0, CreateDefaultSizeAlgorithm(), exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  // The stream is created right above with no type, so its controller is a
  // ReadableStreamDefaultController.
  controller_ =
      static_cast<ReadableStreamDefaultController*>(stream->GetController());
  return stream;
}

void CrossRealmTransformReadable::HandleMessage(MessageType type,
                                                v8::Local<v8::Value> value) {
  // 3. Add a handler for port’s message event with the following steps:
  // The first 5 steps are handled by CrossRealmTransformMessageListener.
  switch (type) {
    // 6. If type is "chunk",
    case MessageType::kChunk:
      // 1. Perform ! ReadableStreamDefaultControllerEnqueue(controller,
      //    value).
      if (ReadableStreamDefaultController::CanCloseOrEnqueue(controller_)) {
        // This can't throw because we always use the default strategy size
        // algorithm, which doesn't throw, and always returns 1.
        bindings::ExceptionState exception_state(isolate_);
        ReadableStreamDefaultController::Enqueue(isolate_, controller_, value,
                                                 exception_state);
      }
      return;
    // 7. Otherwise, if type is "close",
    case MessageType::kClose:
      // 1. Perform ! ReadableStreamDefaultControllerClose(controller).
      if (ReadableStreamDefaultController::CanCloseOrEnqueue(controller_)) {
        ReadableStreamDefaultController::Close(isolate_, controller_);
      }
      // Disentangle port.
      message_port_->Close();
      return;
    // 8. Otherwise, if type is "error",
    case MessageType::kError:
      // 1. Perform ! ReadableStreamDefaultControllerError(controller, value).
      ReadableStreamDefaultController::Error(isolate_, controller_, value);
      // 2. Disentangle port.
      message_port_->Close();
      return;
    default:
      // An invalid message from the peer is ignored.
      return;
  }
}

void CrossRealmTransformReadable::HandleError(v8::Local<v8::Value> error) {
  // 4. Add a handler for port’s messageerror event with the following steps:
  // The first two steps, and the last step, are performed by
  // CrossRealmTransformErrorListener.
  //   3. Perform ! ReadableStreamDefaultControllerError(controller, error).
  ReadableStreamDefaultController::Error(isolate_, controller_, error);
}

}  // namespace

WritableStream* CreateCrossRealmTransformWritable(
    v8::Isolate* isolate,
    MessagePort* port,
    bindings::ExceptionState& exception_state) {
  return bindings::MakeGarbageCollected<CrossRealmTransformWritable>(isolate,
                                                                     port)
      ->CreateWritableStream(exception_state);
}

ReadableStream* CreateCrossRealmTransformReadable(
    v8::Isolate* isolate,
    MessagePort* port,
    bindings::ExceptionState& exception_state) {
  return bindings::MakeGarbageCollected<CrossRealmTransformReadable>(isolate,
                                                                     port)
      ->CreateReadableStream(exception_state);
}

}  // namespace mitto
