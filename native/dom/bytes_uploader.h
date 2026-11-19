// The upload of a fetch()'s body that streams, as Blink's BytesUploader: it
// reads the body's BytesConsumer from when the Session starts reading the
// body, only as fast as the Session takes it, and ends the body, or fails it,
// as the consumer ends or errors, after which it cancels the consumer. Once
// the request has gone, the consumer is cancelled as soon as it has bytes to
// give, as Blink's is once its data pipe has closed.
//
// Upstream: third_party/blink/renderer/core/fetch/bytes_uploader.h
// Upstream: third_party/blink/renderer/core/fetch/bytes_uploader.cc

#ifndef MITTO_DOM_BYTES_UPLOADER_H_
#define MITTO_DOM_BYTES_UPLOADER_H_

#include "base/memory/raw_ptr.h"
#include "mitto/dom/bytes_consumer.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

class FetchController;

class BytesUploader final : public cppgc::GarbageCollected<BytesUploader>,
                            public BytesConsumer::Client {
 public:
  // Uploads `consumer`, which nothing has read, through `controller`, which
  // must outlive this until OnRequestGone() or Detach().
  BytesUploader(BytesConsumer* consumer, FetchController& controller);

  // The Session takes more of the body: the first time, Blink's
  // StartReading(), and after that OnPipeWriteable().
  void OnRoom();
  // The request has ended, or the page let it go. A consumer that has not
  // been read stays as it is, as Blink's does when the network service never
  // asked for the body.
  void OnRequestGone();
  // The request goes with its window, whose context is going: nothing more
  // is read, and no script runs.
  void Detach();

  // BytesConsumer::Client:
  void OnStateChange() override;

  void Trace(cppgc::Visitor* visitor) const override;

 private:
  void WriteDataOnPipe();
  void Close();
  void CloseOnError();
  void Dispose();

  cppgc::Member<BytesConsumer> consumer_;
  // Null once the request has gone, as Blink's data pipe has closed then.
  raw_ptr<FetchController> controller_;
  bool started_ = false;
  bool disposed_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_BYTES_UPLOADER_H_
