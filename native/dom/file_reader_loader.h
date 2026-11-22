// Reads a Blob's bytes for FileReader and for Blob's text(), arrayBuffer()
// and bytes(), as Blink's FileReaderLoader does. Chrome's renderer asks the
// browser for the blob over Mojo and hears its size first, from a task of the
// file reading task source, and its bytes from later tasks, as much as the
// data pipe holds each time, at most 2 MiB; a Page holds the bytes itself, so
// it posts the same tasks with them: one with the size, then one for each
// pipe's worth, and it has finished in the last of them.
//
// Upstream: third_party/blink/renderer/core/fileapi/file_reader_loader.h
// Upstream: third_party/blink/renderer/core/fileapi/file_reader_loader.cc
// Upstream: third_party/blink/renderer/core/fileapi/file_reader_client.h
// Upstream: third_party/blink/renderer/core/fileapi/file_reader_client.cc
// Upstream: third_party/blink/renderer/core/fileapi/file_reader_data.cc
// Upstream: third_party/blink/common/blob/blob_utils.cc

#ifndef MITTO_DOM_FILE_READER_LOADER_H_
#define MITTO_DOM_FILE_READER_LOADER_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>

#include "base/containers/span.h"
#include "base/memory/scoped_refptr.h"
#include "mitto/dom/blob_data.h"
#include "mitto/dom/file_error.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class ExecutionContext;

// What a read gives a page, as Blink's FileReadType.
enum class FileReadType {
  kReadAsArrayBuffer,
  kReadAsBinaryString,
  kReadAsText,
  kReadAsDataURL,
  kReadAsBytes,
};

// What hears how a read goes.
class FileReaderClient : public cppgc::GarbageCollectedMixin {
 public:
  virtual ~FileReaderClient() = default;

  // The read has started and has `total_bytes` to come. Any code but kOK
  // fails it.
  virtual FileErrorCode DidStartLoading(uint64_t total_bytes) = 0;
  // The next bytes. Any code but kOK fails the read.
  virtual FileErrorCode DidReceiveData(base::span<const uint8_t> data) = 0;
  virtual void DidFinishLoading() = 0;
  virtual void DidFail(FileErrorCode code) = 0;
};

// Blink's FileReaderAccumulator: a client that keeps the bytes, and has all of
// them once the read has finished.
class FileReaderAccumulator : public FileReaderClient {
 public:
  FileErrorCode DidStartLoading(uint64_t total_bytes) final;
  FileErrorCode DidReceiveData(base::span<const uint8_t> data) final;
  void DidFinishLoading() final;
  void DidFail(FileErrorCode code) override;

 protected:
  virtual FileErrorCode DidStartLoading();
  virtual FileErrorCode DidReceiveData();
  virtual void DidFinishLoading(std::string bytes) = 0;

 private:
  std::string bytes_;
  uint64_t total_bytes_ = 0;
};

class FileReaderLoader final
    : public cppgc::GarbageCollected<FileReaderLoader> {
 public:
  // A loader that tells `client`, whose tasks are `context`'s.
  FileReaderLoader(FileReaderClient* client, ExecutionContext* context);
  FileReaderLoader(const FileReaderLoader&) = delete;
  FileReaderLoader& operator=(const FileReaderLoader&) = delete;
  ~FileReaderLoader();

  // Starts reading `blob`. A loader reads once.
  void Start(scoped_refptr<BlobDataHandle> blob);
  // Stops it: its client hears nothing more.
  void Cancel();

  bool HasFinishedLoading() const { return finished_loading_; }
  uint64_t BytesLoaded() const { return bytes_loaded_; }
  // How many bytes the read has, once it has started.
  std::optional<uint64_t> TotalBytes() const { return total_bytes_; }

  void Trace(cppgc::Visitor* visitor) const;

 private:
  void OnCalculatedSize();
  void OnDataPipeReadable();
  void Failed(FileErrorCode code);

  cppgc::Member<FileReaderClient> client_;
  cppgc::WeakMember<ExecutionContext> context_;
  scoped_refptr<BlobDataHandle> blob_;
  // How much the data pipe holds, as Blink's BlobUtils::GetDataPipeCapacity()
  // makes it for the blob's size.
  size_t pipe_capacity_ = 0;
  std::optional<uint64_t> total_bytes_;
  uint64_t bytes_loaded_ = 0;
  bool finished_loading_ = false;
  bool cancelled_ = false;
};

// The bytes of a read as a page gets them, as Blink's FileReaderData gives
// them: a binary string of one character for each byte; text, in the encoding
// that `encoding` labels, or UTF-8 when it labels none, after any byte order
// mark, which wins; or a data: URL of `type`, application/octet-stream when it
// is empty, and the bytes in base64.
std::u16string FileReaderResultAsString(std::string_view bytes,
                                        FileReadType read_type,
                                        const std::u16string& encoding,
                                        std::string_view type);

}  // namespace mitto

#endif  // MITTO_DOM_FILE_READER_LOADER_H_
