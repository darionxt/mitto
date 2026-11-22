// The File API's FileReaderSync, which a dedicated worker has, as Blink's:
// each read gives all of a Blob at once, as FileReader's would, from the bytes
// that the worker holds, where Blink waits for the browser's.
//
// Upstream: third_party/blink/renderer/core/fileapi/file_reader_sync.h
// Upstream: third_party/blink/renderer/core/fileapi/file_reader_sync.cc

#ifndef MITTO_DOM_FILE_READER_SYNC_H_
#define MITTO_DOM_FILE_READER_SYNC_H_

#include <optional>
#include <string>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-local-handle.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class Blob;

class FileReaderSync final : public bindings::ScriptWrappable {
 public:
  // `new FileReaderSync()`.
  static FileReaderSync* Create();

  FileReaderSync();
  ~FileReaderSync() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  v8::Local<v8::ArrayBuffer> ReadAsArrayBuffer(
      Blob* blob,
      bindings::ExceptionState& exception_state);
  std::u16string ReadAsBinaryString(Blob* blob,
                                    bindings::ExceptionState& exception_state);
  std::u16string ReadAsText(Blob* blob,
                            const std::optional<std::u16string>& label,
                            bindings::ExceptionState& exception_state);
  std::u16string ReadAsDataURL(Blob* blob,
                               bindings::ExceptionState& exception_state);
};

}  // namespace mitto

#endif  // MITTO_DOM_FILE_READER_SYNC_H_
