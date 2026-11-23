#include "mitto/dom/file_reader_sync.h"

#include <string_view>

#include "base/containers/span.h"
#include "base/strings/string_view_util.h"
#include "mitto/bindings/v8_file_reader_sync.h"
#include "mitto/dom/blob.h"
#include "mitto/dom/file_reader_loader.h"
#include "mitto/dom/stream_chunks.h"
#include "v8/include/v8-isolate.h"

namespace mitto {
namespace {

std::string_view BytesOf(const Blob& blob) {
  return base::as_string_view(blob.GetBlobDataHandle()->Bytes());
}

}  // namespace

// static
FileReaderSync* FileReaderSync::Create() {
  return bindings::MakeGarbageCollected<FileReaderSync>();
}

FileReaderSync::FileReaderSync() = default;
FileReaderSync::~FileReaderSync() = default;

const bindings::WrapperTypeInfo* FileReaderSync::GetWrapperTypeInfo() const {
  return &bindings::V8FileReaderSync::kWrapperTypeInfo;
}

v8::Local<v8::ArrayBuffer> FileReaderSync::ReadAsArrayBuffer(
    Blob* blob,
    bindings::ExceptionState& exception_state) {
  return NewArrayBuffer(v8::Isolate::GetCurrent(),
                        blob->GetBlobDataHandle()->Bytes());
}

std::u16string FileReaderSync::ReadAsBinaryString(
    Blob* blob,
    bindings::ExceptionState& exception_state) {
  return FileReaderResultAsString(BytesOf(*blob),
                                  FileReadType::kReadAsBinaryString,
                                  std::u16string(), std::string_view());
}

std::u16string FileReaderSync::ReadAsText(
    Blob* blob,
    const std::optional<std::u16string>& label,
    bindings::ExceptionState& exception_state) {
  return FileReaderResultAsString(BytesOf(*blob), FileReadType::kReadAsText,
                                  label.value_or(std::u16string()),
                                  std::string_view());
}

std::u16string FileReaderSync::ReadAsDataURL(
    Blob* blob,
    bindings::ExceptionState& exception_state) {
  return FileReaderResultAsString(BytesOf(*blob), FileReadType::kReadAsDataURL,
                                  std::u16string(),
                                  blob->GetBlobDataHandle()->GetType());
}

}  // namespace mitto
