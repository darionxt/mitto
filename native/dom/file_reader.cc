#include "mitto/dom/file_reader.h"

#include <utility>

#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/time/time.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_file_reader.h"
#include "mitto/dom/blob.h"
#include "mitto/dom/dom_exception.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/progress_event.h"
#include "mitto/dom/stream_chunks.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

// Blink's kProgressNotificationInterval.
constexpr base::TimeDelta kProgressNotificationInterval =
    base::Milliseconds(50);

}  // namespace

// static
FileReader* FileReader::Create() {
  return bindings::MakeGarbageCollected<FileReader>(
      &ExecutionContext::Current());
}

FileReader::FileReader(ExecutionContext* context) : context_(context) {}

FileReader::~FileReader() = default;

const bindings::WrapperTypeInfo* FileReader::GetWrapperTypeInfo() const {
  return &bindings::V8FileReader::kWrapperTypeInfo;
}

void FileReader::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(context_);
  visitor->Trace(loader_);
  visitor->Trace(error_);
  visitor->Trace(result_);
  EventTarget::Trace(visitor);
  FileReaderAccumulator::Trace(visitor);
}

void FileReader::ReadAsArrayBuffer(Blob* blob,
                                   bindings::ExceptionState& exception_state) {
  ReadInternal(blob, FileReadType::kReadAsArrayBuffer, exception_state);
}

void FileReader::ReadAsBinaryString(Blob* blob,
                                    bindings::ExceptionState& exception_state) {
  ReadInternal(blob, FileReadType::kReadAsBinaryString, exception_state);
}

void FileReader::ReadAsText(Blob* blob,
                            const std::optional<std::u16string>& encoding,
                            bindings::ExceptionState& exception_state) {
  encoding_ = encoding.value_or(std::u16string());
  ReadInternal(blob, FileReadType::kReadAsText, exception_state);
}

void FileReader::ReadAsDataURL(Blob* blob,
                               bindings::ExceptionState& exception_state) {
  ReadInternal(blob, FileReadType::kReadAsDataURL, exception_state);
}

void FileReader::ReadInternal(Blob* blob,
                              FileReadType type,
                              bindings::ExceptionState& exception_state) {
  // A second read while one is under way throws.
  if (state_ == kLoading) {
    exception_state.ThrowDOMException(
        "InvalidStateError", "The object is already busy reading Blobs.");
    return;
  }
  ExecutionContext* const context = context_.Get();
  if (!context || !context->HasContext()) {
    exception_state.ThrowDOMException(
        "AbortError", "Reading from a detached FileReader is not supported.");
    return;
  }
  if (!observes_context_) {
    observes_context_ = true;
    context->AddContextDestroyedCallback(base::BindOnce(
        [](cppgc::WeakPersistent<FileReader> reader) {
          if (reader) {
            reader->ContextDestroyed();
          }
        },
        cppgc::WeakPersistent<FileReader>(this)));
  }
  // The read takes the blob's bytes as they are now.
  blob_data_handle_ = blob->GetBlobDataHandle();
  blob_type_ = blob_data_handle_->GetType();
  read_type_ = type;
  state_ = kLoading;
  loading_state_ = kLoadingStatePending;
  error_ = nullptr;
  result_.Reset();
  ExecutePendingRead();
}

void FileReader::ExecutePendingRead() {
  DCHECK_EQ(loading_state_, kLoadingStatePending);
  loading_state_ = kLoadingStateLoading;
  loader_ =
      bindings::MakeGarbageCollected<FileReaderLoader>(this, context_.Get());
  loader_->Start(std::move(blob_data_handle_));
}

void FileReader::Abort() {
  if (loading_state_ != kLoadingStateLoading &&
      loading_state_ != kLoadingStatePending) {
    return;
  }
  loading_state_ = kLoadingStateAborted;
  DCHECK_NE(kDone, state_);
  // The loader stops before any event, so that a read that a listener starts
  // finds the reader as it should.
  Terminate();
  // An error makes result null.
  error_ = CreateFileErrorDOMException(FileErrorCode::kAbortErr);
  FireEvent(u"abort");
  if (state_ != kLoading) {
    FireEvent(u"loadend");
  }
}

v8::Local<v8::Value> FileReader::Result() const {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  // The result is there from just before load fires.
  if (error_ || !loader_ || !loader_->HasFinishedLoading() || state_ != kDone ||
      result_.IsEmpty()) {
    return v8::Null(isolate);
  }
  return result_.Get(isolate);
}

void FileReader::Terminate() {
  if (loader_) {
    loader_->Cancel();
    loader_ = nullptr;
  }
  state_ = kDone;
  result_.Reset();
  loading_state_ = kLoadingStateNone;
}

void FileReader::ContextDestroyed() {
  // The abort under way tidies up itself.
  if (loading_state_ == kLoadingStateAborted) {
    return;
  }
  Terminate();
}

FileErrorCode FileReader::DidStartLoading() {
  FireEvent(u"loadstart");
  return FileErrorCode::kOK;
}

FileErrorCode FileReader::DidReceiveData() {
  // Progress fires at most each 50 ms.
  if (!last_progress_notification_time_) {
    last_progress_notification_time_.emplace();
  } else if (last_progress_notification_time_->Elapsed() >
             kProgressNotificationInterval) {
    FireEvent(u"progress");
    last_progress_notification_time_.emplace();
  }
  return FileErrorCode::kOK;
}

void FileReader::DidFinishLoading(std::string bytes) {
  if (loading_state_ == kLoadingStateAborted) {
    return;
  }
  DCHECK_EQ(loading_state_, kLoadingStateLoading);
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (read_type_ == FileReadType::kReadAsArrayBuffer) {
    result_.Reset(isolate, NewArrayBuffer(isolate, base::as_byte_span(bytes)));
  } else {
    result_.Reset(
        isolate, bindings::ToV8String(
                     isolate, FileReaderResultAsString(bytes, read_type_,
                                                       encoding_, blob_type_)));
  }
  // The loading state changes before any event, since a listener may call
  // abort(), which asks whether a read is under way.
  loading_state_ = kLoadingStateNone;
  if (loader_->BytesLoaded() > 0) {
    FireEvent(u"progress");
  }
  DCHECK_NE(kDone, state_);
  state_ = kDone;
  FireEvent(u"load");
  if (state_ != kLoading) {
    FireEvent(u"loadend");
  }
}

void FileReader::DidFail(FileErrorCode code) {
  FileReaderAccumulator::DidFail(code);
  if (loading_state_ == kLoadingStateAborted) {
    return;
  }
  DCHECK_EQ(kLoadingStateLoading, loading_state_);
  loading_state_ = kLoadingStateNone;
  DCHECK_NE(kDone, state_);
  state_ = kDone;
  error_ = CreateFileErrorDOMException(code);
  FireEvent(u"error");
  if (state_ != kLoading) {
    FireEvent(u"loadend");
  }
}

void FileReader::FireEvent(const std::u16string& type) {
  if (!loader_) {
    DispatchEvent(ProgressEvent::CreateTrusted(type, false, 0, 0));
    return;
  }
  if (const std::optional<uint64_t> total = loader_->TotalBytes()) {
    DispatchEvent(ProgressEvent::CreateTrusted(type, true,
                                               loader_->BytesLoaded(), *total));
  } else {
    DispatchEvent(
        ProgressEvent::CreateTrusted(type, false, loader_->BytesLoaded(), 0));
  }
}

}  // namespace mitto
