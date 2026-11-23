// The File API's FileReader, as Blink's: reads a Blob as an ArrayBuffer, a
// binary string, text or a data: URL, through a FileReaderLoader, and fires
// loadstart, progress at most each 50 ms, then load or error, and loadend, or
// abort and loadend when abort() stops it, each a ProgressEvent from a task
// of the file reading task source. Blink runs at most 100 reads of a context
// at once and queues the rest; a Page runs them all.
//
// Upstream: third_party/blink/renderer/core/fileapi/file_reader.h
// Upstream: third_party/blink/renderer/core/fileapi/file_reader.cc

#ifndef MITTO_DOM_FILE_READER_H_
#define MITTO_DOM_FILE_READER_H_

#include <stdint.h>

#include <optional>
#include <string>

#include "base/memory/scoped_refptr.h"
#include "base/timer/elapsed_timer.h"
#include "mitto/dom/blob_data.h"
#include "mitto/dom/event_target.h"
#include "mitto/dom/file_reader_loader.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-persistent-handle.h"

namespace v8 {
class Value;
}  // namespace v8

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class Blob;
class DOMException;
class ExecutionContext;

class FileReader final : public EventTarget, public FileReaderAccumulator {
 public:
  enum ReadyState : uint16_t {
    kEmpty = 0,
    kLoading = 1,
    kDone = 2,
  };

  // `new FileReader()`, of the current context.
  static FileReader* Create();

  explicit FileReader(ExecutionContext* context);
  ~FileReader() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  void ReadAsArrayBuffer(Blob* blob, bindings::ExceptionState& exception_state);
  void ReadAsBinaryString(Blob* blob,
                          bindings::ExceptionState& exception_state);
  void ReadAsText(Blob* blob,
                  const std::optional<std::u16string>& encoding,
                  bindings::ExceptionState& exception_state);
  void ReadAsDataURL(Blob* blob, bindings::ExceptionState& exception_state);
  void Abort();
  uint16_t GetReadyState() const { return state_; }
  v8::Local<v8::Value> Result() const;
  DOMException* Error() const { return error_.Get(); }

 private:
  // How far the read is, as Blink's LoadingState.
  enum LoadingState {
    kLoadingStateNone,
    kLoadingStatePending,
    kLoadingStateLoading,
    kLoadingStateAborted,
  };

  void ReadInternal(Blob* blob,
                    FileReadType type,
                    bindings::ExceptionState& exception_state);
  void ExecutePendingRead();
  // Stops the loader, and leaves the reader done with no result.
  void Terminate();
  // What Blink's ContextDestroyed() does, once the reader's context goes.
  void ContextDestroyed();
  void FireEvent(const std::u16string& type);

  // FileReaderAccumulator:
  FileErrorCode DidStartLoading() override;
  FileErrorCode DidReceiveData() override;
  void DidFinishLoading(std::string bytes) override;
  void DidFail(FileErrorCode code) override;

  cppgc::WeakMember<ExecutionContext> context_;
  ReadyState state_ = kEmpty;
  LoadingState loading_state_ = kLoadingStateNone;
  // Whether it hears when its context goes.
  bool observes_context_ = false;
  FileReadType read_type_ = FileReadType::kReadAsBinaryString;
  std::u16string encoding_;
  std::string blob_type_;
  scoped_refptr<BlobDataHandle> blob_data_handle_;
  cppgc::Member<FileReaderLoader> loader_;
  cppgc::Member<DOMException> error_;
  // What result gives once the load has fired: a string or an ArrayBuffer.
  v8::TracedReference<v8::Value> result_;
  std::optional<base::ElapsedTimer> last_progress_notification_time_;
};

}  // namespace mitto

#endif  // MITTO_DOM_FILE_READER_H_
